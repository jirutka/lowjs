// -----------------------------------------------------------------------------
//  low_loop.cpp
// -----------------------------------------------------------------------------

#include "low_loop.h"
#include "LowLoopCallback.h"

#include "low_config.h"
#include "low_main.h"
#include "low_system.h"

#include <errno.h>

// -----------------------------------------------------------------------------
//  low_loop_run
// -----------------------------------------------------------------------------

#if LOW_ESP32_LWIP_SPECIALITIES
void user_cpu_load(bool active);
#endif /* LOW_ESP32_LWIP_SPECIALITIES */

duk_ret_t low_loop_run_safe(duk_context *ctx, void *udata)
{
    low_main_t *low = duk_get_low_context(ctx);

    while(!low->duk_flag_stop)
    {
        // Handle process.nextTick / low_call_next_tick
        while(!low->duk_flag_stop && duk_get_top(low->next_tick_ctx))
        {
            int num_args = duk_require_int(low->next_tick_ctx, -1);
            duk_pop(low->next_tick_ctx);
            duk_xmove_top(ctx, low->next_tick_ctx, num_args + 1);
            duk_call(ctx, num_args);
            duk_pop_n(ctx, duk_get_top(ctx));
        }

        if(!low->duk_flag_stop && !low->run_ref && !low->loop_callback_first && low->signal_call_id)
        {
            low_push_stash(ctx, low->signal_call_id, false);
            duk_push_string(ctx, "beforeExit");
            duk_call(ctx, 1);
            duk_pop_n(ctx, duk_get_top(ctx));
        }
        if(low->duk_flag_stop || (!low->run_ref && !low->loop_callback_first))
        {
            if(!low->duk_flag_stop && low->signal_call_id)
            {
                low_push_stash(ctx, low->signal_call_id, false);
                duk_push_string(ctx, "exit");
                duk_call(ctx, 1);
                duk_pop_n(ctx, duk_get_top(ctx));
            }
            break;
        }

        int millisecs = -1;
        if(low->chore_times.size())
        {
            int tick_count = low_tick_count();

            auto iter = low->chore_times.lower_bound(low->last_chore_time);
            if(iter == low->chore_times.end())
                iter = low->chore_times.begin();

            millisecs = iter->first - tick_count;
            if(millisecs <= 0)
            {
                low->last_chore_time = iter->first;

                int index = iter->second;
                low->chore_times.erase(iter);

                auto iterData = low->chores.find(index);
                if(iterData->second.oneshot == 2)
                {
                    // C version
                    void (*call)(void *data) = iterData->second.call;
                    void *data = iterData->second.data;
                    if(iterData->second.ref)
                        low->run_ref--;
                    low->chores.erase(iterData);

                    call(data);

                    int index = duk_get_top(ctx);
                    if(index)
                        duk_pop_n(ctx, index);
                }
                else
                {
                    bool erase = iterData->second.oneshot;
                    if(erase)
                    {
                        if(iterData->second.ref)
                            low->run_ref--;
                        low->chores.erase(iterData);
                    }
                    else
                    {
                        iterData->second.stamp += iterData->second.interval;
                        if(iterData->second.stamp - tick_count < 0)
                            iterData->second.stamp = tick_count;

                        low->chore_times.insert(
                        pair<int, int>(iterData->second.stamp, index));
                    }

                    low_push_stash(ctx, index, erase);
                    duk_call(ctx, 0);
                    duk_pop_n(ctx, duk_get_top(ctx));
                }
                continue;
            }
        }

        pthread_mutex_lock(&low->loop_thread_mutex);
        if(!low->loop_callback_first)
        {
            duk_debugger_cooperate(ctx);

#if LOW_ESP32_LWIP_SPECIALITIES
            user_cpu_load(false);
#endif /* LOW_ESP32_LWIP_SPECIALITIES */
            if(millisecs >= 0)
            {
                /*
                // TODO under os x
                pthread_condattr_t attr;
                pthread_cond_t cond;
                pthread_condattr_init(&attr);
                pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
                pthread_cond_init(&cond, &attr);
                */
                struct timespec ts;
                int secs = millisecs / 1000;

#if LOW_ESP32_LWIP_SPECIALITIES
                struct timeval tv;
                gettimeofday(&tv, NULL);
                ts.tv_sec = tv.tv_sec;
                ts.tv_nsec = tv.tv_usec * 1000;
#else
                clock_gettime(CLOCK_REALTIME, &ts);
#endif /* LOW_ESP32_LWIP_SPECIALITIES */
                ts.tv_sec += secs;
                ts.tv_nsec += (millisecs - secs * 1000) * 1000000;
                if(ts.tv_nsec >= 1000000000)
                {
                    ts.tv_sec++;
                    ts.tv_nsec -= 1000000000;
                }

                pthread_cond_timedwait(
                  &low->loop_thread_cond, &low->loop_thread_mutex, &ts);
            }
            else
                pthread_cond_wait(&low->loop_thread_cond,
                                  &low->loop_thread_mutex);
#if LOW_ESP32_LWIP_SPECIALITIES
            user_cpu_load(true);
#endif /* LOW_ESP32_LWIP_SPECIALITIES */
        }
        if(low->loop_callback_first)
        {
            LowLoopCallback *callback = low->loop_callback_first;

            low->loop_callback_first = callback->mNext;
            if(!low->loop_callback_first)
                low->loop_callback_last = NULL;
            callback->mNext = NULL;

            pthread_mutex_unlock(&low->loop_thread_mutex);
            if(!callback->OnLoop())
                delete callback;

            int index = duk_get_top(ctx);
            if(index)
                duk_pop_n(ctx, index);
        }
        else
            pthread_mutex_unlock(&low->loop_thread_mutex);
    }

    return 0;
}

bool low_loop_run(low_main_t *low)
{
    if(duk_safe_call(low->duk_ctx,
                    low_loop_run_safe,
                    NULL,
                    0,
                    1) != DUK_EXEC_SUCCESS)
    {
        if(!low->duk_flag_stop) // flag stop also produces error
            low_duk_print_error(low->duk_ctx);
        duk_pop(low->duk_ctx);
    }

    return low->duk_flag_stop;
}


// -----------------------------------------------------------------------------
//  low_loop_set_chore
// -----------------------------------------------------------------------------

duk_ret_t low_loop_set_chore(duk_context *ctx)
{
    low_main_t *low = duk_get_low_context(ctx);
    int index = low_add_stash(ctx, 0);

    int delay = duk_require_int(ctx, 1);
    if(delay < 0)
        delay = 0;

    low_chore_t &chore = low->chores[index];
    chore.interval = delay;
    chore.stamp = low_tick_count() + chore.interval;
    chore.oneshot = duk_require_boolean(ctx, 2);
    chore.ref = true;
    low->run_ref++;

    low->chore_times.insert(pair<int, int>(chore.stamp, index));
    duk_push_int(ctx, index);
    return 1;
}

// -----------------------------------------------------------------------------
//  low_loop_clear_chore
// -----------------------------------------------------------------------------

duk_ret_t low_loop_clear_chore(duk_context *ctx)
{
    low_main_t *low = duk_get_low_context(ctx);
    int index = duk_require_int(ctx, 0);

    auto iter = low->chores.find(index);
    if(iter == low->chores.end())
        return 0;

    auto iter2 = low->chore_times.find(iter->second.stamp);
    while(iter2->second != index)
        iter2++;
    low->chore_times.erase(iter2);

    if(iter->second.ref)
        low->run_ref--;
    low->chores.erase(iter);
    low_remove_stash(ctx, index);

    return 0;
}


// -----------------------------------------------------------------------------
//  low_set_timeout
// -----------------------------------------------------------------------------

int low_set_timeout(duk_context *ctx, int index, int delay, void (*call)(void *data), void *data)
{
    low_main_t *low = duk_get_low_context(ctx);

    if(index == 0)
    {
        index = low->chores.size() ? low->chores.begin()->first - 1 : -1;
        if(index >= 0)
            index = -1;
    }
    else
    {
        auto iter = low->chores.find(index);
        if(iter != low->chores.end())
        {
            auto iter2 = low->chore_times.find(iter->second.stamp);
            while(iter2->second != index)
                iter2++;
            low->chore_times.erase(iter2);

            if(iter->second.ref)
                low->run_ref--;
        }
    }

    if(delay < 0)
        delay = 0;

    low_chore_t &chore = low->chores[index];
    chore.interval = delay;
    chore.stamp = low_tick_count() + chore.interval;
    chore.oneshot = 2;  // C
    chore.ref = false;
    chore.call = call;
    chore.data = data;

    low->chore_times.insert(pair<int, int>(chore.stamp, index));
    return index;
}


// -----------------------------------------------------------------------------
//  low_clear_timeout
// -----------------------------------------------------------------------------

void low_clear_timeout(duk_context *ctx, int index)
{
    low_main_t *low = duk_get_low_context(ctx);

    auto iter = low->chores.find(index);
    if(iter == low->chores.end())
        return;

    auto iter2 = low->chore_times.find(iter->second.stamp);
    while(iter2->second != index)
        iter2++;
    low->chore_times.erase(iter2);

    if(iter->second.ref)
        low->run_ref--;
    low->chores.erase(iter);
}


// -----------------------------------------------------------------------------
//  low_loop_chore_ref
// -----------------------------------------------------------------------------

duk_ret_t low_loop_chore_ref(duk_context *ctx)
{
    low_main_t *low = duk_get_low_context(ctx);
    int index = duk_require_int(ctx, 0);
    bool ref = duk_require_boolean(ctx, 1);

    auto iter = low->chores.find(index);
    if(iter == low->chores.end())
        return 0;

    if(iter->second.ref != ref)
    {
        if(ref)
            low->run_ref++;
        else
            low->run_ref--;
        iter->second.ref = ref;
    }

    return 0;
}

// -----------------------------------------------------------------------------
//  low_loop_run_ref
// -----------------------------------------------------------------------------

duk_ret_t low_loop_run_ref(duk_context *ctx)
{
    low_main_t *low = duk_get_low_context(ctx);
    low->run_ref += duk_require_int(ctx, 0);
    return 0;
}

// -----------------------------------------------------------------------------
//  low_loop_set_callback
// -----------------------------------------------------------------------------

void low_loop_set_callback(low_main_t *low, LowLoopCallback *callback)
{
    pthread_mutex_lock(&low->loop_thread_mutex);
    if(callback->mNext || low->loop_callback_last == callback)
    {
        pthread_mutex_unlock(&low->loop_thread_mutex);
        return;
    }

    if(low->loop_callback_last)
        low->loop_callback_last->mNext = callback;
    else
        low->loop_callback_first = callback;
    low->loop_callback_last = callback;

    pthread_cond_signal(&low->loop_thread_cond);
    pthread_mutex_unlock(&low->loop_thread_mutex);
}

// -----------------------------------------------------------------------------
//  low_loop_clear_callback
// -----------------------------------------------------------------------------

void low_loop_clear_callback(low_main_t *low, LowLoopCallback *callback)
{
    pthread_mutex_lock(&low->loop_thread_mutex);
    if(callback->mNext || low->loop_callback_last == callback)
    {
        LowLoopCallback *elem = low->loop_callback_first;
        if(elem == callback)
        {
            low->loop_callback_first = elem->mNext;
            if(!low->loop_callback_first)
                low->loop_callback_last = NULL;
        }
        else
        {
            while(elem->mNext != callback)
                elem = elem->mNext;

            if(low->loop_callback_last == callback)
            {
                low->loop_callback_last = elem;
                elem->mNext = NULL;
            }
            else
                elem->mNext = callback->mNext;
        }
        callback->mNext = NULL;
    }
    pthread_mutex_unlock(&low->loop_thread_mutex);
}


// -----------------------------------------------------------------------------
//  low_call_next_tick
// -----------------------------------------------------------------------------

void low_call_next_tick(duk_context *ctx, int num_args)
{
    low_main_t *low = duk_get_low_context(ctx);
    duk_xmove_top(low->next_tick_ctx, ctx, num_args + 1);
    duk_push_int(low->next_tick_ctx, num_args);
}


// -----------------------------------------------------------------------------
//  low_call_next_tick_js
// -----------------------------------------------------------------------------

int low_call_next_tick_js(duk_context *ctx)
{
    low_call_next_tick(ctx, duk_get_top(ctx) - 1);
    return 0;
}