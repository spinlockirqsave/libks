/*
 * Copyright (c) 2018 SignalWire, Inc
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "libks/ks.h"

#define TP_MAX_QLEN 1024

typedef enum {
	TP_STATE_DOWN = 0,
	TP_STATE_RUNNING = 1
} ks_thread_pool_state_t;

struct ks_thread_pool_s {
	uint32_t min;
	uint32_t max;
	uint32_t idle_sec;
	size_t stack_size;
	ks_thread_priority_t priority;
	ks_q_t *q;
	uint32_t thread_count;
	uint32_t busy_thread_count;
	uint32_t running_thread_count;
	uint32_t dying_thread_count;
	ks_thread_pool_state_t state;
	ks_mutex_t *mutex;
};

typedef struct ks_thread_job_s {
	ks_thread_function_t func;
	void *data;
} ks_thread_job_t;


static void *worker_thread(ks_thread_t *thread, void *data);

static int check_queue(ks_thread_pool_t *tp, ks_bool_t adding)
{
	ks_thread_t *thread;
	int need = 0;

	ks_mutex_lock(tp->mutex);

	if (tp->state != TP_STATE_RUNNING) {
		ks_mutex_unlock(tp->mutex);
		return 1;
	}


	if (tp->thread_count < tp->min) {
		need = tp->min - tp->thread_count;
	}

	if (adding) {
		if (!need && tp->busy_thread_count + ks_q_size(tp->q) >= tp->running_thread_count - tp->dying_thread_count &&
			(tp->thread_count - tp->dying_thread_count + 1 <= tp->max)) {
			need++;
		}
	}

	tp->thread_count += need;

	ks_mutex_unlock(tp->mutex);

	while(need > 0) {
		if (ks_thread_create_ex(&thread, worker_thread, tp, KS_THREAD_FLAG_DETACHED, tp->stack_size, tp->priority, ks_pool_get(tp)) != KS_STATUS_SUCCESS) {
			ks_mutex_lock(tp->mutex);
			tp->thread_count--;
			ks_mutex_unlock(tp->mutex);
		}

		need--;
	}
	/*
	ks_log(KS_LOG_DEBUG, "WORKER check: adding %d need %d running %d dying %d total %d max %d\n",
		   adding, need, tp->running_thread_count, tp->dying_thread_count, tp->thread_count, tp->max);
	*/
	return need;
}

static uint32_t TID = 0;

static void *worker_thread(ks_thread_t *thread, void *data)
{
	ks_thread_pool_t *tp = (ks_thread_pool_t *) data;
	uint32_t idle_sec = 0;
	uint32_t my_id = 0;
	int die = 0;

	ks_mutex_lock(tp->mutex);
	tp->running_thread_count++;
	my_id = ++TID;
	ks_mutex_unlock(tp->mutex);

	while(tp->state == TP_STATE_RUNNING) {
		ks_thread_job_t *job;
		void *pop = NULL;
		ks_status_t status;

		status = ks_q_pop_timeout(tp->q, &pop, 100);
		if (status == KS_STATUS_BREAK) {
			if (tp->state != TP_STATE_RUNNING) {
				break;
			}
			continue;
		}

		/*
		ks_log(KS_LOG_DEBUG, "WORKER %d idle_sec %d/%d running %d dying %d total %d max %d\n",
			   my_id, idle_sec, tp->idle_sec, tp->running_thread_count, tp->dying_thread_count, tp->thread_count, tp->max);
		*/

		check_queue(tp, KS_FALSE);

		if (status == KS_STATUS_TIMEOUT) { // || status == KS_STATUS_BREAK) {
			idle_sec++;
			//printf("WTF %d/%d %d,%d,%d %d/%d\n", idle_sec / 10, tp->idle_sec,
			//	   tp->running_thread_count , tp->dying_thread_count , tp->busy_thread_count,
			//	   tp->running_thread_count - tp->dying_thread_count - tp->busy_thread_count, tp->min);
			if (idle_sec / 10 >= tp->idle_sec) {

				ks_mutex_lock(tp->mutex);
				if (tp->running_thread_count - tp->dying_thread_count - tp->busy_thread_count > 0 && tp->running_thread_count > tp->min) {
					tp->dying_thread_count++;
					die = 1;
				}
				ks_mutex_unlock(tp->mutex);

				if (die) {
					break;
				}
			}

			continue;
		}

		if ((status != KS_STATUS_SUCCESS && status != KS_STATUS_BREAK)) {
			ks_log(KS_LOG_ERROR, "WORKER %d POP FAIL %d %p\n", my_id, status, (void *)pop);
			break;
		}

		job = (ks_thread_job_t *) pop;

		ks_mutex_lock(tp->mutex);
		tp->busy_thread_count++;
		ks_mutex_unlock(tp->mutex);

		idle_sec = 0;
		job->func(thread, job->data);

		ks_pool_free(&job);

		ks_mutex_lock(tp->mutex);
		tp->busy_thread_count--;
		ks_mutex_unlock(tp->mutex);
	}

	ks_mutex_lock(tp->mutex);
	tp->running_thread_count--;
	tp->thread_count--;
	if (die) {
		tp->dying_thread_count--;
	}
	ks_mutex_unlock(tp->mutex);

	return NULL;
}

KS_DECLARE(ks_status_t) ks_thread_pool_create(ks_thread_pool_t **tp, uint32_t min, uint32_t max, size_t stack_size,
											  ks_thread_priority_t priority, uint32_t idle_sec)
{
	ks_pool_t *pool = NULL;

	ks_pool_open(&pool);

	*tp = (ks_thread_pool_t *) ks_pool_alloc(pool, sizeof(ks_thread_pool_t));

	(*tp)->min = min;
	(*tp)->max = max;
	(*tp)->stack_size = stack_size;
	(*tp)->priority = priority;
	(*tp)->state = TP_STATE_RUNNING;
	(*tp)->idle_sec = idle_sec;

	ks_mutex_create(&(*tp)->mutex, KS_MUTEX_FLAG_DEFAULT, pool);
	ks_q_create(&(*tp)->q, pool, TP_MAX_QLEN);

	check_queue(*tp, KS_FALSE);

	return KS_STATUS_SUCCESS;

}


KS_DECLARE(ks_status_t) ks_thread_pool_destroy(ks_thread_pool_t **tp)
{
	ks_pool_t *pool = NULL;

	ks_assert(tp);

	(*tp)->state = TP_STATE_DOWN;

	while((*tp)->thread_count) {
		ks_sleep(100000);
	}

	pool = ks_pool_get(*tp);
	ks_pool_close(&pool);

	return KS_STATUS_SUCCESS;
}


KS_DECLARE(ks_status_t) ks_thread_pool_add_job(ks_thread_pool_t *tp, ks_thread_function_t func, void *data)
{
	ks_thread_job_t *job = (ks_thread_job_t *) ks_pool_alloc(ks_pool_get(tp), sizeof(*job));

	job->func = func;
	job->data = data;
	ks_q_push(tp->q, job);

	check_queue(tp, KS_TRUE);

	return KS_STATUS_SUCCESS;
}



KS_DECLARE(ks_size_t) ks_thread_pool_backlog(ks_thread_pool_t *tp)
{
	return ks_q_size(tp->q);
}
