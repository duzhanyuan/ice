// **********************************************************************
//
// Copyright (c) 2003-2006 ZeroC, Inc. All rights reserved.
//
// This copy of Ice is licensed to you under the terms described in the
// ICE_LICENSE file included in this distribution.
//
// **********************************************************************

#include <IceUtil/Random.h>
#include <Ice/IdentityUtil.h>
#include <Ice/LoggerUtil.h>
#include <IceGrid/ObjectCache.h>
#include <IceGrid/NodeSessionI.h>
#include <IceGrid/ServerCache.h>
#include <IceGrid/SessionI.h>

using namespace std;
using namespace IceGrid;

pointer_to_unary_function<int, int> ObjectCache::_rand(IceUtil::random);

namespace IceGrid
{

struct ObjectLoadCI : binary_function<pair<Ice::ObjectPrx, float>&, pair<Ice::ObjectPrx, float>&, bool>
{
    bool operator()(const pair<Ice::ObjectPrx, float>& lhs, const pair<Ice::ObjectPrx, float>& rhs)
    {
	return lhs.second < rhs.second;
    }
};

};

ObjectCache::TypeEntry::TypeEntry(ObjectCache& cache) : _cache(cache)
{
}

void
ObjectCache::TypeEntry::add(const Ice::ObjectPrx& obj)
{
    //
    // No mutex protection here, this is called with the cache locked.
    //
    _objects.insert(lower_bound(_objects.begin(), _objects.end(), obj, ::Ice::proxyIdentityLess), obj);
}

bool
ObjectCache::TypeEntry::remove(const Ice::ObjectPrx& obj)
{
    //
    // No mutex protection here, this is called with the cache locked.
    //
    Ice::ObjectProxySeq::iterator q = lower_bound(_objects.begin(), _objects.end(), obj, ::Ice::proxyIdentityLess);
    assert((*q)->ice_getIdentity() == obj->ice_getIdentity());
    _objects.erase(q);
    return _objects.empty();
}

void
ObjectCache::TypeEntry::addAllocationRequest(const ObjectAllocationRequestPtr& request)
{
    //
    // No mutex protection here, this is called with the cache locked.
    //
    if(request->pending())
    {
	_requests.push_back(request);
    }
}

void
ObjectCache::TypeEntry::released(const ObjectEntryPtr& entry)
{
    //
    // No mutex protection here, this is called with the cache locked.
    //
    while(!_requests.empty() && !entry->isAllocated())
    {
	if(entry->tryAllocate(_requests.front()))
	{
	    _requests.pop_front();
	}
    }
}

ObjectCache::ObjectCache(AdapterCache& adapterCache) : 
    _adapterCache(adapterCache)
{
}

void
ObjectCache::add(const ObjectInfo& info, const string& application, bool allocatable, const AllocatablePtr& parent)
{
    const Ice::Identity& id = info.proxy->ice_getIdentity();

    Lock sync(*this);
    assert(!getImpl(id));

    ObjectEntryPtr entry = new ObjectEntry(*this, info, application, allocatable, parent);
    addImpl(id, entry);

    map<string, TypeEntry>::iterator p = _types.find(entry->getType());
    if(p == _types.end())
    {
	p = _types.insert(p, make_pair(entry->getType(), TypeEntry(*this)));
    }
    p->second.add(info.proxy);

    if(_traceLevels && _traceLevels->object > 0)
    {
	Ice::Trace out(_traceLevels->logger, _traceLevels->objectCat);
	out << "added object `" << Ice::identityToString(id) << "'";	
    }    
}

ObjectEntryPtr
ObjectCache::get(const Ice::Identity& id) const
{
    Lock sync(*this);
    ObjectEntryPtr entry = getImpl(id);
    if(!entry)
    {
	throw ObjectNotRegisteredException(id);
    }
    return entry;
}

ObjectEntryPtr
ObjectCache::remove(const Ice::Identity& id)
{
    Lock sync(*this);
    ObjectEntryPtr entry = removeImpl(id);
    assert(entry);

    map<string, TypeEntry>::iterator p = _types.find(entry->getType());
    assert(p != _types.end());
    if(p->second.remove(entry->getObjectInfo().proxy))
    {	
	_types.erase(p);
    }

    if(_traceLevels && _traceLevels->object > 0)
    {
	Ice::Trace out(_traceLevels->logger, _traceLevels->objectCat);
	out << "removed object `" << Ice::identityToString(id) << "'";	
    }    

    return entry;
}

void
ObjectCache::allocateByType(const string& type, const ObjectAllocationRequestPtr& request)
{
    Lock sync(*this);
    map<string, TypeEntry>::iterator p = _types.find(type);
    if(p == _types.end())
    {
	request->response(0);
	return;
    }

    Ice::ObjectProxySeq objects = p->second.getObjects();
    random_shuffle(objects.begin(), objects.end(), _rand); // TODO: OPTIMIZE
    for(Ice::ObjectProxySeq::const_iterator q = objects.begin(); q != objects.end(); ++q)
    {
	//
	// If tryAllocate() returns true, either the object was
	// successfully allocated or the request canceled. In both
	// cases, we're done!
	//
	if(getImpl((*q)->ice_getIdentity())->tryAllocate(request))
	{
	    return;
	}
    }
    
    p->second.addAllocationRequest(request);
}

void
ObjectCache::allocateByTypeOnLeastLoadedNode(const string& type, 
					     const ObjectAllocationRequestPtr& request, 
					     LoadSample sample)
{
    Lock sync(*this);
    map<string, TypeEntry>::iterator p = _types.find(type);
    if(p == _types.end())
    {
	request->response(0);
	return;
    }

    Ice::ObjectProxySeq objects = p->second.getObjects();
    random_shuffle(objects.begin(), objects.end(), _rand); // TODO: OPTIMIZE
    vector<pair<Ice::ObjectPrx, float> > objsWLoad;
    objsWLoad.reserve(objects.size());
    for(Ice::ObjectProxySeq::const_iterator o = objects.begin(); o != objects.end(); ++o)
    {
	float load = 1.0f;
	if(!(*o)->ice_getAdapterId().empty())
	{
	    try
	    {
		load = _adapterCache.get((*o)->ice_getAdapterId())->getLeastLoadedNodeLoad(sample);
	    }
	    catch(const AdapterNotExistException&)
	    {
	    }
	}
	objsWLoad.push_back(make_pair(*o, load));
    }
    sort(objsWLoad.begin(), objsWLoad.end(), ObjectLoadCI());

    for(vector<pair<Ice::ObjectPrx, float> >::const_iterator q = objsWLoad.begin(); q != objsWLoad.end(); ++q)
    {
	//
	// If tryAllocate() returns true, either the object was
	// successfully allocated or the request canceled. In both
	// cases, we're done!
	//
	if(getImpl(q->first->ice_getIdentity())->tryAllocate(request))
	{
	    return;
	}
    }
    
    p->second.addAllocationRequest(request);
}

void
ObjectCache::released(const ObjectEntryPtr& entry)
{
    //
    // Notify the type entry that an object was released.
    //
    Lock sync(*this);
    map<string, TypeEntry>::iterator p = _types.find(entry->getType());
    if(p == _types.end())
    {
	return;
    }
    p->second.released(entry);
}

Ice::ObjectProxySeq
ObjectCache::getObjectsByType(const string& type)
{
    Lock sync(*this);
    Ice::ObjectProxySeq proxies;
    map<string, TypeEntry>::const_iterator p = _types.find(type);
    if(p == _types.end())
    {
	return proxies;
    }
    const Ice::ObjectProxySeq& objects = p->second.getObjects();
    for(Ice::ObjectProxySeq::const_iterator q = objects.begin(); q != objects.end(); ++q)
    {
	ObjectEntryPtr entry = getImpl((*q)->ice_getIdentity());
	if(!entry->allocatable()) // Only return non-allocatable objects.
	{
	    proxies.push_back(*q);
	}
    }
    return proxies;
}

ObjectInfoSeq
ObjectCache::getAll(const string& expression)
{
    Lock sync(*this);
    ObjectInfoSeq infos;
    for(map<Ice::Identity, ObjectEntryPtr>::const_iterator p = _entries.begin(); p != _entries.end(); ++p)
    {
	if(expression.empty() || IceUtil::match(Ice::identityToString(p->first), expression, true))
	{
	    infos.push_back(p->second->getObjectInfo());
	}
    }
    return infos;
}

ObjectInfoSeq
ObjectCache::getAllByType(const string& type)
{
    Lock sync(*this);
    ObjectInfoSeq infos;
    map<string, TypeEntry>::const_iterator p = _types.find(type);
    if(p == _types.end())
    {
	return infos;
    }
    const Ice::ObjectProxySeq& objects = p->second.getObjects();
    for(Ice::ObjectProxySeq::const_iterator q = objects.begin(); q != objects.end(); ++q)
    {
	infos.push_back(getImpl((*q)->ice_getIdentity())->getObjectInfo());
    }
    return infos;
}

ObjectEntry::ObjectEntry(ObjectCache& cache, 
			 const ObjectInfo& info, 
			 const string& application, 
			 bool allocatable,
			 const AllocatablePtr& parent) :
    Allocatable(allocatable, parent),
    _cache(cache),
    _info(info),
    _application(application)
{
}

Ice::ObjectPrx
ObjectEntry::getProxy(const SessionIPtr&) const
{
    //
    // TODO: Remove this code if we really don't want to check the
    // session for allocatable objects.
    //
//     if(allocatable())
//     {
// 	return getSession() == session ? _info.proxy : Ice::ObjectPrx();
//     }
//     else
//     {
    return _info.proxy;
//     }
}

string
ObjectEntry::getType() const
{
    return _info.type;
}

string
ObjectEntry::getApplication() const
{
    return _application;
}

const ObjectInfo&
ObjectEntry::getObjectInfo() const
{
    return _info;
}

bool
ObjectEntry::canRemove()
{
    return true;
}

bool
ObjectEntry::release(const SessionIPtr& session)
{
    std::set<AllocatablePtr> releasedAllocatables;
    if(Allocatable::release(session, true, releasedAllocatables))
    {
	//
	// Notify the cache that this entry was released. Note that we
	// don't use the released callback here. This could lead to
	// deadlocks since released() is called with the allocation
	// mutex locked.
	//
	_cache.released(this);

	//
	// Notify the cache that other entries were released. For
	// example, if it's the adapter which was allocated, all its
	// objects are potentially released.
	//
	std::set<AllocatablePtr>::const_iterator p;
	for(p = releasedAllocatables.begin(); p != releasedAllocatables.end(); ++p)
	{
	    _cache.released(ObjectEntryPtr::dynamicCast(*p));
	}
	return true;
    }
    return false;
}

bool
ObjectEntry::allocated(const SessionIPtr& session)
{
    //
    // Add the object allocation to the session. The object will be
    // released once the session is destroyed.
    //
    if(!session->addAllocation(this))
    {
	return false;
    }

    TraceLevelsPtr traceLevels = _cache.getTraceLevels();
    if(traceLevels && traceLevels->object > 1)
    {
	Ice::Trace out(traceLevels->logger, traceLevels->objectCat);
	const Ice::Identity id = _info.proxy->ice_getIdentity();
	out << "object `" << id << "' allocated by `" << session->getUserId() << "' (" << _count << ")";
    }    

    return true;
}

void
ObjectEntry::released(const SessionIPtr& session)
{
    //
    // Remove the object allocation from the session.
    //
    session->removeAllocation(this);

    TraceLevelsPtr traceLevels = _cache.getTraceLevels();
    if(traceLevels && traceLevels->object > 1)
    {
	Ice::Trace out(traceLevels->logger, traceLevels->objectCat);
	const Ice::Identity id = _info.proxy->ice_getIdentity();
	out << "object `" << id << "' released by `" << session->getUserId() << "' (" << _count << ")";
    }    
}
