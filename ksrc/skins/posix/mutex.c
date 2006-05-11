/*
 * Written by Gilles Chanteperdrix <gilles.chanteperdrix@laposte.net>.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 */

/**
 * @ingroup posix
 * @defgroup posix_mutex Mutex services.
 *
 * Mutex services.
 *
 * A mutex is a MUTual EXclusion device, and is useful for protecting
 * shared data structures from concurrent modifications, and implementing
 * critical sections and monitors.
 *
 * A mutex has two possible states: unlocked (not owned by any thread), and
 * locked (owned by one thread). A mutex can never be owned by two different
 * threads simultaneously. A thread attempting to lock a mutex that is already
 * locked by another thread is suspended until the owning thread unlocks the
 * mutex first.
 *
 * Before it can be used, a mutex has to be initialized with
 * pthread_mutex_init(). An attribute object, which reference may be passed to
 * this service, allows to select the features of the created mutex, namely its
 * @a type (see pthread_mutexattr_settype()) and the priority @a protocol it
 * uses (see pthread_mutexattr_setprotocol()).
 *
 * There is no support for the @a pshared attribute; mutexes created by Xenomai
 * POSIX skin may be shared by kernel-space modules and user-space processes
 * through shared memory.
 *
 * By default, Xenomai POSIX skin mutexes are of the recursive type and use the
 * priority inheritance protocol.
 *
 * Note that only pthread_mutex_init() may be used to initialize a mutex, using
 * the static initializer @a PTHREAD_MUTEX_INITIALIZER is not supported.
 *
 *@{*/

#include <posix/mutex.h>

static pthread_mutexattr_t default_attr;

static xnqueue_t pse51_mutexq;

static void pse51_mutex_destroy_internal (pse51_mutex_t *mutex)

{
    removeq(&pse51_mutexq, &mutex->link);
    /* synchbase wait queue may not be empty only when this function is called
       from pse51_mutex_pkg_cleanup, hence the absence of xnpod_schedule(). */
    xnsynch_destroy(&mutex->synchbase);
    xnfree(mutex);
}

void pse51_mutex_pkg_init (void)

{
    initq(&pse51_mutexq);
    pthread_mutexattr_init(&default_attr);
}

void pse51_mutex_pkg_cleanup (void)

{
    xnholder_t *holder;
    spl_t s;

    xnlock_get_irqsave(&nklock, s);

    while ((holder = getheadq(&pse51_mutexq)) != NULL)
        {
#ifdef CONFIG_XENO_OPT_DEBUG
        xnprintf("Posix mutex %p was not destroyed, destroying now.\n",
                 link2mutex(holder));
#endif /* CONFIG_XENO_OPT_DEBUG */
	pse51_mutex_destroy_internal(link2mutex(holder));
        }

    xnlock_put_irqrestore(&nklock, s);
}

/**
 * Initialize a mutex.
 *
 * This services initializes the mutex @a mx, using the mutex attributes object
 * @a attr. If @a attr is @a NULL or this service is used from user-space,
 * default attributes are used (see pthread_mutexattr_init()).
 *
 * @param mx the mutex to be initialized;
 *
 * @param attr the mutex attributes object.
 *
 * @return 0 on success,
 * @return an error number if:
 * - EINVAL, the mutex attributes object @a attr is invalid or uninitialized;
 * - EBUSY, the mutex @a mx was already initialized;
 * - ENOMEM, insufficient memory exists in the system heap to initialize the
 *   mutex, increase CONFIG_XENO_OPT_SYS_HEAPSZ.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutex_init.html">
 * Specification.</a>
 * 
 */
int pthread_mutex_init (pthread_mutex_t *mx, const pthread_mutexattr_t *attr)
{
    struct __shadow_mutex *shadow = &((union __xeno_mutex *) mx)->shadow_mutex;
    xnflags_t synch_flags = XNSYNCH_PRIO | XNSYNCH_NOPIP;
    pse51_mutex_t *mutex;
    spl_t s;
    
    if (!attr)
        attr = &default_attr;

    xnlock_get_irqsave(&nklock, s);

    if (attr->magic != PSE51_MUTEX_ATTR_MAGIC)
	{
        xnlock_put_irqrestore(&nklock, s);
        return EINVAL;
	}

    if (shadow->magic == PSE51_MUTEX_MAGIC)
        {
        xnholder_t *holder;
        for(holder = getheadq(&pse51_mutexq); holder;
            holder = nextq(&pse51_mutexq, holder))
            if (holder == &shadow->mutex->link)
                {
                /* mutex is already in the queue. */
                xnlock_put_irqrestore(&nklock, s);
                return EBUSY;
                }
        }

    mutex = (pse51_mutex_t *) xnmalloc(sizeof(*mutex));
    if (!mutex)
        {
        xnlock_put_irqrestore(&nklock, s);
        return ENOMEM;
        }

    shadow->magic = PSE51_MUTEX_MAGIC;
    shadow->mutex = mutex;

    if (attr->protocol == PTHREAD_PRIO_INHERIT)
        synch_flags |= XNSYNCH_PIP;
    
    xnsynch_init(&mutex->synchbase, synch_flags);
    inith(&mutex->link);
    mutex->attr = *attr;
    mutex->count = 0;

    appendq(&pse51_mutexq, &mutex->link);

    xnlock_put_irqrestore(&nklock, s);

    return 0;
}

/**
 * Destroy a mutex.
 *
 * This service destroys the mutex @a mx, if it is unlocked and not referenced
 * by any condition variable. The mutex becomes invalid for all mutex services
 * (they all return the EINVAL error) except pthread_mutex_init().
 *
 * @param mx the mutex to be destroyed.
 *
 * @return 0 on success,
 * @return an error number if:
 * - EINVAL, the mutex @a mx is invalid;
 * - EBUSY, the mutex is locked, or used by a condition variable.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutex_destroy.html">
 * Specification.</a>
 * 
 */
int pthread_mutex_destroy (pthread_mutex_t *mx)

{
    struct __shadow_mutex *shadow = &((union __xeno_mutex *) mx)->shadow_mutex;
    pse51_mutex_t *mutex;
    spl_t s;

    xnlock_get_irqsave(&nklock, s);

    if (!pse51_obj_active(shadow, PSE51_MUTEX_MAGIC, struct __shadow_mutex))
	{
        xnlock_put_irqrestore(&nklock, s);
        return EINVAL;
	}

    mutex = shadow->mutex;

    if (mutex->count || mutex->condvars)
	{
        xnlock_put_irqrestore(&nklock, s);
        return EBUSY;
	}

    pse51_mark_deleted(shadow);
    pse51_mutex_destroy_internal(mutex);
    
    xnlock_put_irqrestore(&nklock, s);

    return 0;
}

int pse51_mutex_timedlock_break (struct __shadow_mutex *shadow, xnticks_t abs_to)

{
    xnthread_t *cur = xnpod_current_thread();
    pse51_mutex_t *mutex;
    int err;
    spl_t s;

    xnlock_get_irqsave(&nklock, s);

    err = mutex_timedlock_internal(cur, shadow, abs_to);

    if (err == EBUSY)
        {
        mutex = shadow->mutex;

        switch (mutex->attr.type)
	    {
	    case PTHREAD_MUTEX_NORMAL:
		/* Deadlock. */
		for (;;)
                    {
                    xnticks_t to = abs_to;

                    err = clock_adjust_timeout(&to, CLOCK_REALTIME);

                    if (err)
                        break;
                    
		    xnsynch_sleep_on(&mutex->synchbase, to);

		    if (xnthread_test_flags(cur, XNBREAK))
                        {
                        err = EINTR;
                        break;
                        }

		    if (xnthread_test_flags(cur, XNTIMEO))
                        {
                        err = ETIMEDOUT;
                        break;
                        }

		    if (xnthread_test_flags(cur, XNRMID))
                        {
                        err = EINVAL;
                        break;
                        }

		    mutex->count = 1;
                    }

            break;

        case PTHREAD_MUTEX_ERRORCHECK:

            err = EDEADLK;
            break;

        case PTHREAD_MUTEX_RECURSIVE:

            if (mutex->count == UINT_MAX)
		{
                err = EAGAIN;
                break;
		}
                
            ++mutex->count;
            err = 0;
	    }
        }

    xnlock_put_irqrestore(&nklock, s);

    return err;
}

/**
 * Attempt to lock a mutex.
 *
 * This service is equivalent to pthread_mutex_lock(), except that if the mutex
 * @a mx is locked by another thread than the current one, this service returns
 * immediately.
 *
 * @param mx the mutex to be locked.
 *
 * @return 0 on success;
 * @return an error number if:
 * - EPERM, the caller context is invalid;
 * - EINVAL, the mutex is invalid;
 * - EBUSY, the mutex was locked by another thread than the current one;
 * - EAGAIN, the mutex is recursive, and the maximum number of recursive locks
 *   has been exceeded.
 *
 * @par Valid contexts:
 * - Xenomai kernel-space thread,
 * - Xenomai user-space thread (switches to primary mode).
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutex_trylock.html">
 * Specification.</a>
 * 
 */
int pthread_mutex_trylock (pthread_mutex_t *mx)

{
    struct __shadow_mutex *shadow = &((union __xeno_mutex *) mx)->shadow_mutex;
    xnthread_t *cur = xnpod_current_thread();
    int err;
    spl_t s;
    
    xnlock_get_irqsave(&nklock, s);

    err = mutex_trylock_internal(cur, shadow);

    if (err == EBUSY)
        {
        pse51_mutex_t *mutex = shadow->mutex;

        if (mutex->attr.type == PTHREAD_MUTEX_RECURSIVE
            && xnsynch_owner(&mutex->synchbase) == cur)
            {
            if (mutex->count == UINT_MAX)
                err = EAGAIN;
            else
                {
                ++mutex->count;
                err = 0;
                }
            }
        }

    xnlock_put_irqrestore(&nklock, s);

    return err;
}

/**
 * Lock a mutex.
 *
 * This service attempts to lock the mutex @a mx. If the mutex is free, it
 * becomes locked. If it was locked by another thread than the current one, the
 * current thread is suspended until the mutex is unlocked. If it was already
 * locked by the current mutex, the behaviour of this service depends on the
 * mutex type :
 * - for mutexes of the @a PTHREAD_MUTEX_NORMAL type, this service deadlocks;
 * - for mutexes of the @a PTHREAD_MUTEX_ERRORCHECK type, this service returns
 *   the EDEADLK error number;
 * - for mutexes of the @a PTHREAD_MUTEX_RECURSIVE type, this service increments
 *   the lock recursion count and returns 0.
 *
 * @param mx the mutex to be locked.
 *
 * @return 0 on success
 * @return an error number if:
 * - EPERM, the caller context is invalid;
 * - EINVAL, the mutex @a mx is invalid;
 * - EDEADLK, the mutex is of the @a PTHREAD_MUTEX_ERRORCHECK type and the mutex
 *   was already locked by the current thread;
 * - EAGAIN, the mutex is of the @a PTHREAD_MUTEX_RECURSIVE type and the maximum
 *   number of recursive locks has been exceeded.
 *
 * @par Valid contexts:
 * - Xenomai kernel-space thread;
 * - Xenomai user-space thread (switches to primary mode).
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutex_lock.html">
 * Specification.</a>
 *
 */
int pthread_mutex_lock (pthread_mutex_t *mx)

{
    struct __shadow_mutex *shadow = &((union __xeno_mutex *) mx)->shadow_mutex;
    int err;

    do {
        err = pse51_mutex_timedlock_break(shadow, XN_INFINITE);
    } while(err == EINTR);

    return err;
}

/**
 * Attempt, during a bounded time, to lock a mutex.
 *
 * This service is equivalent to pthread_mutex_lock(), except that if the mutex
 * @a mx is locked by another thread than the current one, this service only
 * suspends the current thread until the timeout specified by @a to expires.
 *
 * @param mx the mutex to be locked;
 *
 * @param to the timeout, expressed as an absolute value of the CLOCK_REALTIME
 * clock.
 *
 * @return 0 on success;
 * @return an error number if:
 * - EPERM, the caller context is invalid;
 * - EINVAL, the mutex @a mx is invalid;
 * - ETIMEDOUT, the mutex could not be locked and the specified timeout
 *   expired;
 * - EDEADLK, the mutex is of the @a PTHREAD_MUTEX_ERRORCHECK type and the mutex
 *   was already locked by the current thread;
 * - EAGAIN, the mutex is of the @a PTHREAD_MUTEX_RECURSIVE type and the maximum
 *   number of recursive locks has been exceeded.
 *
 * @par Valid contexts:
 * - Xenomai kernel-space thread;
 * - Xenomai user-space thread (switches to primary mode).
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutex_timedlock.html">
 * Specification.</a>
 *
 */
int pthread_mutex_timedlock (pthread_mutex_t *mx, const struct timespec *to)

{
    struct __shadow_mutex *shadow = &((union __xeno_mutex *) mx)->shadow_mutex;
    int err;

    do {
        err = pse51_mutex_timedlock_break(shadow, ts2ticks_ceil(to)+1);
    } while(err == EINTR);

    return err;
}

/* must be called with nklock locked, interrupts off. */
static inline int mutex_unlock_internal(xnthread_t *cur,
                                        struct __shadow_mutex *shadow)

{
    pse51_mutex_t *mutex;

    if (!pse51_obj_active(shadow, PSE51_MUTEX_MAGIC, struct __shadow_mutex))
        return EINVAL;

    mutex = shadow->mutex;
 
    if (xnsynch_owner(&mutex->synchbase) != cur || mutex->count != 1)
        return EPERM;
    
    mutex->count = 0;
    if (xnsynch_wakeup_one_sleeper(&mutex->synchbase))
        xnpod_schedule();

    return 0;
}

/**
 * Unlock a mutex.
 *
 * This service unlocks the mutex @a mx. If the mutex is of the @a
 * PTHREAD_MUTEX_RECURSIVE @a type and the locking recursion count is greater
 * than one, the lock recursion count is decremented and the mutex remains
 * locked.
 *
 * Attempting to unlock a mutex which is not locked or which is locked by
 * another thread than the current one yields the EPERM error, whatever the
 * mutex @a type attribute.
 *
 * @param mx the mutex to be released.
 *
 * @return 0 on success;
 * @return an error number if:
 * - EPERM, the caller context is invalid;
 * - EINVAL, the mutex @a mx is invalid;
 * - EPERM, the mutex was not locked by the current thread.
 *
 * @par Valid contexts:
 * - Xenomai kernel-space thread,
 * - kernel-space cancellation cleanup routine,
 * - Xenomai user-space thread (switches to primary mode),
 * - user-space cancellation cleanup routine.
 *
 * @see
 * <a href="http://www.opengroup.org/onlinepubs/000095399/functions/pthread_mutex_unlock.html">
 * Specification.</a>
 * 
 */
int pthread_mutex_unlock (pthread_mutex_t *mx)

{
    struct __shadow_mutex *shadow = &((union __xeno_mutex *) mx)->shadow_mutex;
    xnthread_t *cur = xnpod_current_thread();
    int err;
    spl_t s;

    if (xnpod_root_p() || xnpod_interrupt_p())
        return EPERM;

    xnlock_get_irqsave(&nklock, s);

    err = mutex_unlock_internal(cur, shadow);

    if (err == EPERM)
        {
        pse51_mutex_t *mutex = shadow->mutex;

        if(mutex->attr.type == PTHREAD_MUTEX_RECURSIVE
           && xnsynch_owner(&mutex->synchbase) == cur
           && mutex->count)
            {
            --mutex->count;
            err = 0;
            }
        }

    xnlock_put_irqrestore(&nklock, s);

    return err;
}

/*@}*/

EXPORT_SYMBOL(pthread_mutex_init);
EXPORT_SYMBOL(pthread_mutex_destroy);
EXPORT_SYMBOL(pthread_mutex_trylock);
EXPORT_SYMBOL(pthread_mutex_lock);
EXPORT_SYMBOL(pthread_mutex_timedlock);
EXPORT_SYMBOL(pthread_mutex_unlock);
