// **********************************************************************
//
// Copyright (c) 2003-2017 ZeroC, Inc. All rights reserved.
//
// This copy of Ice is licensed to you under the terms described in the
// ICE_LICENSE file included in this distribution.
//
// **********************************************************************

package com.zeroc.IceBT;

/**
 * Plug-in factories must implement this interface.
 **/
public class PluginFactory implements com.zeroc.Ice.PluginFactory
{
    /**
     * Returns a new plug-in.
     *
     * @param communicator The communicator for the plug-in.
     * @param name The name of the plug-in.
     * @param args The arguments that are specified in the plug-in's configuration.
     *
     * @return The new plug-in. <code>null</code> can be returned to indicate
     * that a general error occurred. Alternatively, <code>create</code> can throw
     * PluginInitializationException to provide more detailed information.
     **/
    @Override
    public com.zeroc.Ice.Plugin create(com.zeroc.Ice.Communicator communicator, String name, String[] args)
    {
        return new PluginI(communicator);
    }
}