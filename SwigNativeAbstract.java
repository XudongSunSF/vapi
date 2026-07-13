package com.wellsfargo.vasara.jni;

/**
 * Base for SWIG-generated native proxy classes (e.g. Handle).
 *
 * Declared abstract so it is never instantiated on its own: a bare
 * SwigNativeAbstract would expose the no-op delete() below as if it freed a
 * native resource, silently doing nothing. Concrete SWIG proxies override
 * delete() with a real implementation and call super.delete() as the terminal
 * of the chain, so the no-op body must remain.
 */
public abstract class SwigNativeAbstract implements SwigNative
{
    public synchronized void delete()
    {
    }
}
