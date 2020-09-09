/*
 * Copyright © 2016 Intel Corporation
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice (including the next
 * paragraph) shall be included in all copies or substantial portions of the
 * Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
 * IN THE SOFTWARE.
 *
 */

#include <linux/prime_numbers.h>
#include <linux/pm_qos.h>

#include "gem/i915_gem_pm.h"
#include "gem/selftests/mock_context.h"

#include "gt/intel_engine_pm.h"
#include "gt/intel_engine_user.h"
#include "gt/intel_gt.h"

#include "i915_random.h"
#include "i915_selftest.h"
#include "igt_live_test.h"
#include "igt_spinner.h"
#include "lib_sw_fence.h"

#include "mock_drm.h"
#include "mock_gem_device.h"

static unsigned int num_uabi_engines(struct drm_i915_private *i915)
{
	struct intel_engine_cs *engine;
	unsigned int count;

	count = 0;
	for_each_uabi_engine(engine, i915)
		count++;

	return count;
}

static struct intel_engine_cs *rcs0(struct drm_i915_private *i915)
{
	return intel_engine_lookup_user(i915, I915_ENGINE_CLASS_RENDER, 0);
}

static int igt_add_request(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct i915_request *request;

	/* Basic preliminary test to create a request and let it loose! */

	request = mock_request(rcs0(i915)->kernel_context, HZ / 10);
	if (!request)
		return -ENOMEM;

	i915_request_add(request);

	return 0;
}

static int igt_wait_request(void *arg)
{
	const long T = HZ / 4;
	struct drm_i915_private *i915 = arg;
	struct i915_request *request;
	int err = -EINVAL;

	/* Submit a request, then wait upon it */

	request = mock_request(rcs0(i915)->kernel_context, T);
	if (!request)
		return -ENOMEM;

	i915_request_get(request);

	if (i915_request_wait(request, 0, 0) != -ETIME) {
		pr_err("request wait (busy query) succeeded (expected timeout before submit!)\n");
		goto out_request;
	}

	if (i915_request_wait(request, 0, T) != -ETIME) {
		pr_err("request wait succeeded (expected timeout before submit!)\n");
		goto out_request;
	}

	if (i915_request_completed(request)) {
		pr_err("request completed before submit!!\n");
		goto out_request;
	}

	i915_request_add(request);

	if (i915_request_wait(request, 0, 0) != -ETIME) {
		pr_err("request wait (busy query) succeeded (expected timeout after submit!)\n");
		goto out_request;
	}

	if (i915_request_completed(request)) {
		pr_err("request completed immediately!\n");
		goto out_request;
	}

	if (i915_request_wait(request, 0, T / 2) != -ETIME) {
		pr_err("request wait succeeded (expected timeout!)\n");
		goto out_request;
	}

	if (i915_request_wait(request, 0, T) == -ETIME) {
		pr_err("request wait timed out!\n");
		goto out_request;
	}

	if (!i915_request_completed(request)) {
		pr_err("request not complete after waiting!\n");
		goto out_request;
	}

	if (i915_request_wait(request, 0, T) == -ETIME) {
		pr_err("request wait timed out when already complete!\n");
		goto out_request;
	}

	err = 0;
out_request:
	i915_request_put(request);
	mock_device_flush(i915);
	return err;
}

static int igt_fence_wait(void *arg)
{
	const long T = HZ / 4;
	struct drm_i915_private *i915 = arg;
	struct i915_request *request;
	int err = -EINVAL;

	/* Submit a request, treat it as a fence and wait upon it */

	request = mock_request(rcs0(i915)->kernel_context, T);
	if (!request)
		return -ENOMEM;

	if (dma_fence_wait_timeout(&request->fence, false, T) != -ETIME) {
		pr_err("fence wait success before submit (expected timeout)!\n");
		goto out;
	}

	i915_request_add(request);

	if (dma_fence_is_signaled(&request->fence)) {
		pr_err("fence signaled immediately!\n");
		goto out;
	}

	if (dma_fence_wait_timeout(&request->fence, false, T / 2) != -ETIME) {
		pr_err("fence wait success after submit (expected timeout)!\n");
		goto out;
	}

	if (dma_fence_wait_timeout(&request->fence, false, T) <= 0) {
		pr_err("fence wait timed out (expected success)!\n");
		goto out;
	}

	if (!dma_fence_is_signaled(&request->fence)) {
		pr_err("fence unsignaled after waiting!\n");
		goto out;
	}

	if (dma_fence_wait_timeout(&request->fence, false, T) <= 0) {
		pr_err("fence wait timed out when complete (expected success)!\n");
		goto out;
	}

	err = 0;
out:
	mock_device_flush(i915);
	return err;
}

static int igt_request_rewind(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct i915_request *request, *vip;
	struct i915_gem_context *ctx[2];
	struct intel_context *ce;
	int err = -EINVAL;

	ctx[0] = mock_context(i915, "A");

	ce = i915_gem_context_get_engine(ctx[0], RCS0);
	GEM_BUG_ON(IS_ERR(ce));
	request = mock_request(ce, 2 * HZ);
	intel_context_put(ce);
	if (!request) {
		err = -ENOMEM;
		goto err_context_0;
	}

	i915_request_get(request);
	i915_request_add(request);

	ctx[1] = mock_context(i915, "B");

	ce = i915_gem_context_get_engine(ctx[1], RCS0);
	GEM_BUG_ON(IS_ERR(ce));
	vip = mock_request(ce, 0);
	intel_context_put(ce);
	if (!vip) {
		err = -ENOMEM;
		goto err_context_1;
	}

	/* Simulate preemption by manual reordering */
	if (!mock_cancel_request(request)) {
		pr_err("failed to cancel request (already executed)!\n");
		i915_request_add(vip);
		goto err_context_1;
	}
	i915_request_get(vip);
	i915_request_add(vip);
	rcu_read_lock();
	request->engine->submit_request(request);
	rcu_read_unlock();


	if (i915_request_wait(vip, 0, HZ) == -ETIME) {
		pr_err("timed out waiting for high priority request\n");
		goto err;
	}

	if (i915_request_completed(request)) {
		pr_err("low priority request already completed\n");
		goto err;
	}

	err = 0;
err:
	i915_request_put(vip);
err_context_1:
	mock_context_close(ctx[1]);
	i915_request_put(request);
err_context_0:
	mock_context_close(ctx[0]);
	mock_device_flush(i915);
	return err;
}

struct smoketest {
	struct intel_engine_cs *engine;
	struct i915_gem_context **contexts;
	atomic_long_t num_waits, num_fences;
	int ncontexts, max_batch;
	struct i915_request *(*request_alloc)(struct intel_context *ce);
};

static struct i915_request *
__mock_request_alloc(struct intel_context *ce)
{
	return mock_request(ce, 0);
}

static struct i915_request *
__live_request_alloc(struct intel_context *ce)
{
	return intel_context_create_request(ce);
}

static int __igt_breadcrumbs_smoketest(void *arg)
{
	struct smoketest *t = arg;
	const unsigned int max_batch = min(t->ncontexts, t->max_batch) - 1;
	const unsigned int total = 4 * t->ncontexts + 1;
	unsigned int num_waits = 0, num_fences = 0;
	struct i915_request **requests;
	I915_RND_STATE(prng);
	unsigned int *order;
	int err = 0;

	/*
	 * A very simple test to catch the most egregious of list handling bugs.
	 *
	 * At its heart, we simply create oodles of requests running across
	 * multiple kthreads and enable signaling on them, for the sole purpose
	 * of stressing our breadcrumb handling. The only inspection we do is
	 * that the fences were marked as signaled.
	 */

	requests = kcalloc(total, sizeof(*requests), GFP_KERNEL);
	if (!requests)
		return -ENOMEM;

	order = i915_random_order(total, &prng);
	if (!order) {
		err = -ENOMEM;
		goto out_requests;
	}

	while (!kthread_should_stop()) {
		struct i915_sw_fence *submit, *wait;
		unsigned int n, count;

		submit = heap_fence_create(GFP_KERNEL);
		if (!submit) {
			err = -ENOMEM;
			break;
		}

		wait = heap_fence_create(GFP_KERNEL);
		if (!wait) {
			i915_sw_fence_commit(submit);
			heap_fence_put(submit);
			err = ENOMEM;
			break;
		}

		i915_random_reorder(order, total, &prng);
		count = 1 + i915_prandom_u32_max_state(max_batch, &prng);

		for (n = 0; n < count; n++) {
			struct i915_gem_context *ctx =
				t->contexts[order[n] % t->ncontexts];
			struct i915_request *rq;
			struct intel_context *ce;

			ce = i915_gem_context_get_engine(ctx, t->engine->legacy_idx);
			GEM_BUG_ON(IS_ERR(ce));
			rq = t->request_alloc(ce);
			intel_context_put(ce);
			if (IS_ERR(rq)) {
				err = PTR_ERR(rq);
				count = n;
				break;
			}

			err = i915_sw_fence_await_sw_fence_gfp(&rq->submit,
							       submit,
							       GFP_KERNEL);

			requests[n] = i915_request_get(rq);
			i915_request_add(rq);

			if (err >= 0)
				err = i915_sw_fence_await_dma_fence(wait,
								    &rq->fence,
								    0,
								    GFP_KERNEL);

			if (err < 0) {
				i915_request_put(rq);
				count = n;
				break;
			}
		}

		i915_sw_fence_commit(submit);
		i915_sw_fence_commit(wait);

		if (!wait_event_timeout(wait->wait,
					i915_sw_fence_done(wait),
					5 * HZ)) {
			struct i915_request *rq = requests[count - 1];

			pr_err("waiting for %d/%d fences (last %llx:%lld) on %s timed out!\n",
			       atomic_read(&wait->pending), count,
			       rq->fence.context, rq->fence.seqno,
			       t->engine->name);
			GEM_TRACE_DUMP();

			intel_gt_set_wedged(t->engine->gt);
			GEM_BUG_ON(!i915_request_completed(rq));
			i915_sw_fence_wait(wait);
			err = -EIO;
		}

		for (n = 0; n < count; n++) {
			struct i915_request *rq = requests[n];

			if (!test_bit(DMA_FENCE_FLAG_SIGNALED_BIT,
				      &rq->fence.flags)) {
				pr_err("%llu:%llu was not signaled!\n",
				       rq->fence.context, rq->fence.seqno);
				err = -EINVAL;
			}

			i915_request_put(rq);
		}

		heap_fence_put(wait);
		heap_fence_put(submit);

		if (err < 0)
			break;

		num_fences += count;
		num_waits++;

		cond_resched();
	}

	atomic_long_add(num_fences, &t->num_fences);
	atomic_long_add(num_waits, &t->num_waits);

	kfree(order);
out_requests:
	kfree(requests);
	return err;
}

static int mock_breadcrumbs_smoketest(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct smoketest t = {
		.engine = rcs0(i915),
		.ncontexts = 1024,
		.max_batch = 1024,
		.request_alloc = __mock_request_alloc
	};
	unsigned int ncpus = num_online_cpus();
	struct task_struct **threads;
	unsigned int n;
	int ret = 0;

	/*
	 * Smoketest our breadcrumb/signal handling for requests across multiple
	 * threads. A very simple test to only catch the most egregious of bugs.
	 * See __igt_breadcrumbs_smoketest();
	 */

	threads = kcalloc(ncpus, sizeof(*threads), GFP_KERNEL);
	if (!threads)
		return -ENOMEM;

	t.contexts = kcalloc(t.ncontexts, sizeof(*t.contexts), GFP_KERNEL);
	if (!t.contexts) {
		ret = -ENOMEM;
		goto out_threads;
	}

	for (n = 0; n < t.ncontexts; n++) {
		t.contexts[n] = mock_context(t.engine->i915, "mock");
		if (!t.contexts[n]) {
			ret = -ENOMEM;
			goto out_contexts;
		}
	}

	for (n = 0; n < ncpus; n++) {
		threads[n] = kthread_run(__igt_breadcrumbs_smoketest,
					 &t, "igt/%d", n);
		if (IS_ERR(threads[n])) {
			ret = PTR_ERR(threads[n]);
			ncpus = n;
			break;
		}

		get_task_struct(threads[n]);
	}

	yield(); /* start all threads before we begin */
	msleep(jiffies_to_msecs(i915_selftest.timeout_jiffies));

	for (n = 0; n < ncpus; n++) {
		int err;

		err = kthread_stop(threads[n]);
		if (err < 0 && !ret)
			ret = err;

		put_task_struct(threads[n]);
	}
	pr_info("Completed %lu waits for %lu fence across %d cpus\n",
		atomic_long_read(&t.num_waits),
		atomic_long_read(&t.num_fences),
		ncpus);

out_contexts:
	for (n = 0; n < t.ncontexts; n++) {
		if (!t.contexts[n])
			break;
		mock_context_close(t.contexts[n]);
	}
	kfree(t.contexts);
out_threads:
	kfree(threads);
	return ret;
}

int i915_request_mock_selftests(void)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(igt_add_request),
		SUBTEST(igt_wait_request),
		SUBTEST(igt_fence_wait),
		SUBTEST(igt_request_rewind),
		SUBTEST(mock_breadcrumbs_smoketest),
	};
	struct drm_i915_private *i915;
	intel_wakeref_t wakeref;
	int err = 0;

	i915 = mock_gem_device();
	if (!i915)
		return -ENOMEM;

	with_intel_runtime_pm(&i915->runtime_pm, wakeref)
		err = i915_subtests(tests, i915);

	drm_dev_put(&i915->drm);

	return err;
}

static int live_nop_request(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct intel_engine_cs *engine;
	struct igt_live_test t;
	int err = -ENODEV;

	/*
	 * Submit various sized batches of empty requests, to each engine
	 * (individually), and wait for the batch to complete. We can check
	 * the overhead of submitting requests to the hardware.
	 */

	for_each_uabi_engine(engine, i915) {
		unsigned long n, prime;
		IGT_TIMEOUT(end_time);
		ktime_t times[2] = {};

		err = igt_live_test_begin(&t, i915, __func__, engine->name);
		if (err)
			return err;

		intel_engine_pm_get(engine);
		for_each_prime_number_from(prime, 1, 8192) {
			struct i915_request *request = NULL;

			times[1] = ktime_get_raw();

			for (n = 0; n < prime; n++) {
				i915_request_put(request);
				request = i915_request_create(engine->kernel_context);
				if (IS_ERR(request))
					return PTR_ERR(request);

				/*
				 * This space is left intentionally blank.
				 *
				 * We do not actually want to perform any
				 * action with this request, we just want
				 * to measure the latency in allocation
				 * and submission of our breadcrumbs -
				 * ensuring that the bare request is sufficient
				 * for the system to work (i.e. proper HEAD
				 * tracking of the rings, interrupt handling,
				 * etc). It also gives us the lowest bounds
				 * for latency.
				 */

				i915_request_get(request);
				i915_request_add(request);
			}
			i915_request_wait(request, 0, MAX_SCHEDULE_TIMEOUT);
			i915_request_put(request);

			times[1] = ktime_sub(ktime_get_raw(), times[1]);
			if (prime == 1)
				times[0] = times[1];

			if (__igt_timeout(end_time, NULL))
				break;
		}
		intel_engine_pm_put(engine);

		err = igt_live_test_end(&t);
		if (err)
			return err;

		pr_info("Request latencies on %s: 1 = %lluns, %lu = %lluns\n",
			engine->name,
			ktime_to_ns(times[0]),
			prime, div64_u64(ktime_to_ns(times[1]), prime));
	}

	return err;
}

static struct i915_vma *empty_batch(struct drm_i915_private *i915)
{
	struct drm_i915_gem_object *obj;
	struct i915_vma *vma;
	u32 *cmd;
	int err;

	obj = i915_gem_object_create_internal(i915, PAGE_SIZE);
	if (IS_ERR(obj))
		return ERR_CAST(obj);

	cmd = i915_gem_object_pin_map(obj, I915_MAP_WB);
	if (IS_ERR(cmd)) {
		err = PTR_ERR(cmd);
		goto err;
	}

	*cmd = MI_BATCH_BUFFER_END;

	__i915_gem_object_flush_map(obj, 0, 64);
	i915_gem_object_unpin_map(obj);

	intel_gt_chipset_flush(&i915->gt);

	vma = i915_vma_instance(obj, &i915->ggtt.vm, NULL);
	if (IS_ERR(vma)) {
		err = PTR_ERR(vma);
		goto err;
	}

	err = i915_vma_pin(vma, 0, 0, PIN_USER | PIN_GLOBAL);
	if (err)
		goto err;

	/* Force the wait wait now to avoid including it in the benchmark */
	err = i915_vma_sync(vma);
	if (err)
		goto err_pin;

	return vma;

err_pin:
	i915_vma_unpin(vma);
err:
	i915_gem_object_put(obj);
	return ERR_PTR(err);
}

static struct i915_request *
empty_request(struct intel_engine_cs *engine,
	      struct i915_vma *batch)
{
	struct i915_request *request;
	int err;

	request = i915_request_create(engine->kernel_context);
	if (IS_ERR(request))
		return request;

	err = engine->emit_bb_start(request,
				    batch->node.start,
				    batch->node.size,
				    I915_DISPATCH_SECURE);
	if (err)
		goto out_request;

	i915_request_get(request);
out_request:
	i915_request_add(request);
	return err ? ERR_PTR(err) : request;
}

static int live_empty_request(void *arg)
{
	struct drm_i915_private *i915 = arg;
	struct intel_engine_cs *engine;
	struct igt_live_test t;
	struct i915_vma *batch;
	int err = 0;

	/*
	 * Submit various sized batches of empty requests, to each engine
	 * (individually), and wait for the batch to complete. We can check
	 * the overhead of submitting requests to the hardware.
	 */

	batch = empty_batch(i915);
	if (IS_ERR(batch))
		return PTR_ERR(batch);

	for_each_uabi_engine(engine, i915) {
		IGT_TIMEOUT(end_time);
		struct i915_request *request;
		unsigned long n, prime;
		ktime_t times[2] = {};

		err = igt_live_test_begin(&t, i915, __func__, engine->name);
		if (err)
			goto out_batch;

		intel_engine_pm_get(engine);

		/* Warmup / preload */
		request = empty_request(engine, batch);
		if (IS_ERR(request)) {
			err = PTR_ERR(request);
			intel_engine_pm_put(engine);
			goto out_batch;
		}
		i915_request_wait(request, 0, MAX_SCHEDULE_TIMEOUT);

		for_each_prime_number_from(prime, 1, 8192) {
			times[1] = ktime_get_raw();

			for (n = 0; n < prime; n++) {
				i915_request_put(request);
				request = empty_request(engine, batch);
				if (IS_ERR(request)) {
					err = PTR_ERR(request);
					intel_engine_pm_put(engine);
					goto out_batch;
				}
			}
			i915_request_wait(request, 0, MAX_SCHEDULE_TIMEOUT);

			times[1] = ktime_sub(ktime_get_raw(), times[1]);
			if (prime == 1)
				times[0] = times[1];

			if (__igt_timeout(end_time, NULL))
				break;
		}
		i915_request_put(request);
		intel_engine_pm_put(engine);

		err = igt_live_test_end(&t);
		if (err)
			goto out_batch;

		pr_info("Batch latencies on %s: 1 = %lluns, %lu = %lluns\n",
			engine->name,
			ktime_to_ns(times[0]),
			prime, div64_u64(ktime_to_ns(times[1]), prime));
	}

out_batch:
	i915_vma_unpin(batch);
	i915_vma_put(batch);
	return err;
}

static struct i915_vma *recursive_batch(struct drm_i915_private *i915)
{
	struct drm_i915_gem_object *obj;
	const int gen = INTEL_GEN(i915);
	struct i915_vma *vma;
	u32 *cmd;
	int err;

	obj = i915_gem_object_create_internal(i915, PAGE_SIZE);
	if (IS_ERR(obj))
		return ERR_CAST(obj);

	vma = i915_vma_instance(obj, i915->gt.vm, NULL);
	if (IS_ERR(vma)) {
		err = PTR_ERR(vma);
		goto err;
	}

	err = i915_vma_pin(vma, 0, 0, PIN_USER);
	if (err)
		goto err;

	cmd = i915_gem_object_pin_map(obj, I915_MAP_WC);
	if (IS_ERR(cmd)) {
		err = PTR_ERR(cmd);
		goto err;
	}

	if (gen >= 8) {
		*cmd++ = MI_BATCH_BUFFER_START | 1 << 8 | 1;
		*cmd++ = lower_32_bits(vma->node.start);
		*cmd++ = upper_32_bits(vma->node.start);
	} else if (gen >= 6) {
		*cmd++ = MI_BATCH_BUFFER_START | 1 << 8;
		*cmd++ = lower_32_bits(vma->node.start);
	} else {
		*cmd++ = MI_BATCH_BUFFER_START | MI_BATCH_GTT;
		*cmd++ = lower_32_bits(vma->node.start);
	}
	*cmd++ = MI_BATCH_BUFFER_END; /* terminate early in case of error */

	__i915_gem_object_flush_map(obj, 0, 64);
	i915_gem_object_unpin_map(obj);

	intel_gt_chipset_flush(&i915->gt);

	return vma;

err:
	i915_gem_object_put(obj);
	return ERR_PTR(err);
}

static int recursive_batch_resolve(struct i915_vma *batch)
{
	u32 *cmd;

	cmd = i915_gem_object_pin_map(batch->obj, I915_MAP_WC);
	if (IS_ERR(cmd))
		return PTR_ERR(cmd);

	*cmd = MI_BATCH_BUFFER_END;

	__i915_gem_object_flush_map(batch->obj, 0, sizeof(*cmd));
	i915_gem_object_unpin_map(batch->obj);

	intel_gt_chipset_flush(batch->vm->gt);

	return 0;
}

static int live_all_engines(void *arg)
{
	struct drm_i915_private *i915 = arg;
	const unsigned int nengines = num_uabi_engines(i915);
	struct intel_engine_cs *engine;
	struct i915_request **request;
	struct igt_live_test t;
	struct i915_vma *batch;
	unsigned int idx;
	int err;

	/*
	 * Check we can submit requests to all engines simultaneously. We
	 * send a recursive batch to each engine - checking that we don't
	 * block doing so, and that they don't complete too soon.
	 */

	request = kcalloc(nengines, sizeof(*request), GFP_KERNEL);
	if (!request)
		return -ENOMEM;

	err = igt_live_test_begin(&t, i915, __func__, "");
	if (err)
		goto out_free;

	batch = recursive_batch(i915);
	if (IS_ERR(batch)) {
		err = PTR_ERR(batch);
		pr_err("%s: Unable to create batch, err=%d\n", __func__, err);
		goto out_free;
	}

	idx = 0;
	for_each_uabi_engine(engine, i915) {
		request[idx] = intel_engine_create_kernel_request(engine);
		if (IS_ERR(request[idx])) {
			err = PTR_ERR(request[idx]);
			pr_err("%s: Request allocation failed with err=%d\n",
			       __func__, err);
			goto out_request;
		}

		i915_vma_lock(batch);
		err = i915_request_await_object(request[idx], batch->obj, 0);
		if (err == 0)
			err = i915_vma_move_to_active(batch, request[idx], 0);
		i915_vma_unlock(batch);
		GEM_BUG_ON(err);

		err = engine->emit_bb_start(request[idx],
					    batch->node.start,
					    batch->node.size,
					    0);
		GEM_BUG_ON(err);
		request[idx]->batch = batch;

		i915_request_get(request[idx]);
		i915_request_add(request[idx]);
		idx++;
	}

	idx = 0;
	for_each_uabi_engine(engine, i915) {
		if (i915_request_completed(request[idx])) {
			pr_err("%s(%s): request completed too early!\n",
			       __func__, engine->name);
			err = -EINVAL;
			goto out_request;
		}
		idx++;
	}

	err = recursive_batch_resolve(batch);
	if (err) {
		pr_err("%s: failed to resolve batch, err=%d\n", __func__, err);
		goto out_request;
	}

	idx = 0;
	for_each_uabi_engine(engine, i915) {
		long timeout;

		timeout = i915_request_wait(request[idx], 0,
					    MAX_SCHEDULE_TIMEOUT);
		if (timeout < 0) {
			err = timeout;
			pr_err("%s: error waiting for request on %s, err=%d\n",
			       __func__, engine->name, err);
			goto out_request;
		}

		GEM_BUG_ON(!i915_request_completed(request[idx]));
		i915_request_put(request[idx]);
		request[idx] = NULL;
		idx++;
	}

	err = igt_live_test_end(&t);

out_request:
	idx = 0;
	for_each_uabi_engine(engine, i915) {
		if (request[idx])
			i915_request_put(request[idx]);
		idx++;
	}
	i915_vma_unpin(batch);
	i915_vma_put(batch);
out_free:
	kfree(request);
	return err;
}

static int live_sequential_engines(void *arg)
{
	struct drm_i915_private *i915 = arg;
	const unsigned int nengines = num_uabi_engines(i915);
	struct i915_request **request;
	struct i915_request *prev = NULL;
	struct intel_engine_cs *engine;
	struct igt_live_test t;
	unsigned int idx;
	int err;

	/*
	 * Check we can submit requests to all engines sequentially, such
	 * that each successive request waits for the earlier ones. This
	 * tests that we don't execute requests out of order, even though
	 * they are running on independent engines.
	 */

	request = kcalloc(nengines, sizeof(*request), GFP_KERNEL);
	if (!request)
		return -ENOMEM;

	err = igt_live_test_begin(&t, i915, __func__, "");
	if (err)
		goto out_free;

	idx = 0;
	for_each_uabi_engine(engine, i915) {
		struct i915_vma *batch;

		batch = recursive_batch(i915);
		if (IS_ERR(batch)) {
			err = PTR_ERR(batch);
			pr_err("%s: Unable to create batch for %s, err=%d\n",
			       __func__, engine->name, err);
			goto out_free;
		}

		request[idx] = intel_engine_create_kernel_request(engine);
		if (IS_ERR(request[idx])) {
			err = PTR_ERR(request[idx]);
			pr_err("%s: Request allocation failed for %s with err=%d\n",
			       __func__, engine->name, err);
			goto out_request;
		}

		if (prev) {
			err = i915_request_await_dma_fence(request[idx],
							   &prev->fence);
			if (err) {
				i915_request_add(request[idx]);
				pr_err("%s: Request await failed for %s with err=%d\n",
				       __func__, engine->name, err);
				goto out_request;
			}
		}

		i915_vma_lock(batch);
		err = i915_request_await_object(request[idx],
						batch->obj, false);
		if (err == 0)
			err = i915_vma_move_to_active(batch, request[idx], 0);
		i915_vma_unlock(batch);
		GEM_BUG_ON(err);

		err = engine->emit_bb_start(request[idx],
					    batch->node.start,
					    batch->node.size,
					    0);
		GEM_BUG_ON(err);
		request[idx]->batch = batch;

		i915_request_get(request[idx]);
		i915_request_add(request[idx]);

		prev = request[idx];
		idx++;
	}

	idx = 0;
	for_each_uabi_engine(engine, i915) {
		long timeout;

		if (i915_request_completed(request[idx])) {
			pr_err("%s(%s): request completed too early!\n",
			       __func__, engine->name);
			err = -EINVAL;
			goto out_request;
		}

		err = recursive_batch_resolve(request[idx]->batch);
		if (err) {
			pr_err("%s: failed to resolve batch, err=%d\n",
			       __func__, err);
			goto out_request;
		}

		timeout = i915_request_wait(request[idx], 0,
					    MAX_SCHEDULE_TIMEOUT);
		if (timeout < 0) {
			err = timeout;
			pr_err("%s: error waiting for request on %s, err=%d\n",
			       __func__, engine->name, err);
			goto out_request;
		}

		GEM_BUG_ON(!i915_request_completed(request[idx]));
		idx++;
	}

	err = igt_live_test_end(&t);

out_request:
	idx = 0;
	for_each_uabi_engine(engine, i915) {
		u32 *cmd;

		if (!request[idx])
			break;

		cmd = i915_gem_object_pin_map(request[idx]->batch->obj,
					      I915_MAP_WC);
		if (!IS_ERR(cmd)) {
			*cmd = MI_BATCH_BUFFER_END;

			__i915_gem_object_flush_map(request[idx]->batch->obj,
						    0, sizeof(*cmd));
			i915_gem_object_unpin_map(request[idx]->batch->obj);

			intel_gt_chipset_flush(engine->gt);
		}

		i915_vma_put(request[idx]->batch);
		i915_request_put(request[idx]);
		idx++;
	}
out_free:
	kfree(request);
	return err;
}

static int __live_parallel_engine1(void *arg)
{
	struct intel_engine_cs *engine = arg;
	IGT_TIMEOUT(end_time);
	unsigned long count;
	int err = 0;

	count = 0;
	intel_engine_pm_get(engine);
	do {
		struct i915_request *rq;

		rq = i915_request_create(engine->kernel_context);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			break;
		}

		i915_request_get(rq);
		i915_request_add(rq);

		err = 0;
		if (i915_request_wait(rq, 0, HZ / 5) < 0)
			err = -ETIME;
		i915_request_put(rq);
		if (err)
			break;

		count++;
	} while (!__igt_timeout(end_time, NULL));
	intel_engine_pm_put(engine);

	pr_info("%s: %lu request + sync\n", engine->name, count);
	return err;
}

static int __live_parallel_engineN(void *arg)
{
	struct intel_engine_cs *engine = arg;
	IGT_TIMEOUT(end_time);
	unsigned long count;
	int err = 0;

	count = 0;
	intel_engine_pm_get(engine);
	do {
		struct i915_request *rq;

		rq = i915_request_create(engine->kernel_context);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			break;
		}

		i915_request_add(rq);
		count++;
	} while (!__igt_timeout(end_time, NULL));
	intel_engine_pm_put(engine);

	pr_info("%s: %lu requests\n", engine->name, count);
	return err;
}

static bool wake_all(struct drm_i915_private *i915)
{
	if (atomic_dec_and_test(&i915->selftest.counter)) {
		wake_up_var(&i915->selftest.counter);
		return true;
	}

	return false;
}

static int wait_for_all(struct drm_i915_private *i915)
{
	if (wake_all(i915))
		return 0;

	if (wait_var_event_timeout(&i915->selftest.counter,
				   !atomic_read(&i915->selftest.counter),
				   i915_selftest.timeout_jiffies))
		return 0;

	return -ETIME;
}

static int __live_parallel_spin(void *arg)
{
	struct intel_engine_cs *engine = arg;
	struct igt_spinner spin;
	struct i915_request *rq;
	int err = 0;

	/*
	 * Create a spinner running for eternity on each engine. If a second
	 * spinner is incorrectly placed on the same engine, it will not be
	 * able to start in time.
	 */

	if (igt_spinner_init(&spin, engine->gt)) {
		wake_all(engine->i915);
		return -ENOMEM;
	}

	intel_engine_pm_get(engine);
	rq = igt_spinner_create_request(&spin,
					engine->kernel_context,
					MI_NOOP); /* no preemption */
	intel_engine_pm_put(engine);
	if (IS_ERR(rq)) {
		err = PTR_ERR(rq);
		if (err == -ENODEV)
			err = 0;
		wake_all(engine->i915);
		goto out_spin;
	}

	i915_request_get(rq);
	i915_request_add(rq);
	if (igt_wait_for_spinner(&spin, rq)) {
		/* Occupy this engine for the whole test */
		err = wait_for_all(engine->i915);
	} else {
		pr_err("Failed to start spinner on %s\n", engine->name);
		err = -EINVAL;
	}
	igt_spinner_end(&spin);

	if (err == 0 && i915_request_wait(rq, 0, HZ / 5) < 0)
		err = -EIO;
	i915_request_put(rq);

out_spin:
	igt_spinner_fini(&spin);
	return err;
}

static int live_parallel_engines(void *arg)
{
	struct drm_i915_private *i915 = arg;
	static int (* const func[])(void *arg) = {
		__live_parallel_engine1,
		__live_parallel_engineN,
		__live_parallel_spin,
		NULL,
	};
	const unsigned int nengines = num_uabi_engines(i915);
	struct intel_engine_cs *engine;
	int (* const *fn)(void *arg);
	struct task_struct **tsk;
	int err = 0;

	/*
	 * Check we can submit requests to all engines concurrently. This
	 * tests that we load up the system maximally.
	 */

	tsk = kcalloc(nengines, sizeof(*tsk), GFP_KERNEL);
	if (!tsk)
		return -ENOMEM;

	for (fn = func; !err && *fn; fn++) {
		char name[KSYM_NAME_LEN];
		struct igt_live_test t;
		unsigned int idx;

		snprintf(name, sizeof(name), "%ps", *fn);
		err = igt_live_test_begin(&t, i915, __func__, name);
		if (err)
			break;

		atomic_set(&i915->selftest.counter, nengines);

		idx = 0;
		for_each_uabi_engine(engine, i915) {
			tsk[idx] = kthread_run(*fn, engine,
					       "igt/parallel:%s",
					       engine->name);
			if (IS_ERR(tsk[idx])) {
				err = PTR_ERR(tsk[idx]);
				break;
			}
			get_task_struct(tsk[idx++]);
		}

		yield(); /* start all threads before we kthread_stop() */

		idx = 0;
		for_each_uabi_engine(engine, i915) {
			int status;

			if (IS_ERR(tsk[idx]))
				break;

			status = kthread_stop(tsk[idx]);
			if (status && !err)
				err = status;

			put_task_struct(tsk[idx++]);
		}

		if (igt_live_test_end(&t))
			err = -EIO;
	}

	kfree(tsk);
	return err;
}

static int
max_batches(struct i915_gem_context *ctx, struct intel_engine_cs *engine)
{
	struct i915_request *rq;
	int ret;

	/*
	 * Before execlists, all contexts share the same ringbuffer. With
	 * execlists, each context/engine has a separate ringbuffer and
	 * for the purposes of this test, inexhaustible.
	 *
	 * For the global ringbuffer though, we have to be very careful
	 * that we do not wrap while preventing the execution of requests
	 * with a unsignaled fence.
	 */
	if (HAS_EXECLISTS(ctx->i915))
		return INT_MAX;

	rq = igt_request_alloc(ctx, engine);
	if (IS_ERR(rq)) {
		ret = PTR_ERR(rq);
	} else {
		int sz;

		ret = rq->ring->size - rq->reserved_space;
		i915_request_add(rq);

		sz = rq->ring->emit - rq->head;
		if (sz < 0)
			sz += rq->ring->size;
		ret /= sz;
		ret /= 2; /* leave half spare, in case of emergency! */
	}

	return ret;
}

static int live_breadcrumbs_smoketest(void *arg)
{
	struct drm_i915_private *i915 = arg;
	const unsigned int nengines = num_uabi_engines(i915);
	const unsigned int ncpus = num_online_cpus();
	unsigned long num_waits, num_fences;
	struct intel_engine_cs *engine;
	struct task_struct **threads;
	struct igt_live_test live;
	intel_wakeref_t wakeref;
	struct smoketest *smoke;
	unsigned int n, idx;
	struct file *file;
	int ret = 0;

	/*
	 * Smoketest our breadcrumb/signal handling for requests across multiple
	 * threads. A very simple test to only catch the most egregious of bugs.
	 * See __igt_breadcrumbs_smoketest();
	 *
	 * On real hardware this time.
	 */

	wakeref = intel_runtime_pm_get(&i915->runtime_pm);

	file = mock_file(i915);
	if (IS_ERR(file)) {
		ret = PTR_ERR(file);
		goto out_rpm;
	}

	smoke = kcalloc(nengines, sizeof(*smoke), GFP_KERNEL);
	if (!smoke) {
		ret = -ENOMEM;
		goto out_file;
	}

	threads = kcalloc(ncpus * nengines, sizeof(*threads), GFP_KERNEL);
	if (!threads) {
		ret = -ENOMEM;
		goto out_smoke;
	}

	smoke[0].request_alloc = __live_request_alloc;
	smoke[0].ncontexts = 64;
	smoke[0].contexts = kcalloc(smoke[0].ncontexts,
				    sizeof(*smoke[0].contexts),
				    GFP_KERNEL);
	if (!smoke[0].contexts) {
		ret = -ENOMEM;
		goto out_threads;
	}

	for (n = 0; n < smoke[0].ncontexts; n++) {
		smoke[0].contexts[n] = live_context(i915, file);
		if (!smoke[0].contexts[n]) {
			ret = -ENOMEM;
			goto out_contexts;
		}
	}

	ret = igt_live_test_begin(&live, i915, __func__, "");
	if (ret)
		goto out_contexts;

	idx = 0;
	for_each_uabi_engine(engine, i915) {
		smoke[idx] = smoke[0];
		smoke[idx].engine = engine;
		smoke[idx].max_batch =
			max_batches(smoke[0].contexts[0], engine);
		if (smoke[idx].max_batch < 0) {
			ret = smoke[idx].max_batch;
			goto out_flush;
		}
		/* One ring interleaved between requests from all cpus */
		smoke[idx].max_batch /= num_online_cpus() + 1;
		pr_debug("Limiting batches to %d requests on %s\n",
			 smoke[idx].max_batch, engine->name);

		for (n = 0; n < ncpus; n++) {
			struct task_struct *tsk;

			tsk = kthread_run(__igt_breadcrumbs_smoketest,
					  &smoke[idx], "igt/%d.%d", idx, n);
			if (IS_ERR(tsk)) {
				ret = PTR_ERR(tsk);
				goto out_flush;
			}

			get_task_struct(tsk);
			threads[idx * ncpus + n] = tsk;
		}

		idx++;
	}

	yield(); /* start all threads before we begin */
	msleep(jiffies_to_msecs(i915_selftest.timeout_jiffies));

out_flush:
	idx = 0;
	num_waits = 0;
	num_fences = 0;
	for_each_uabi_engine(engine, i915) {
		for (n = 0; n < ncpus; n++) {
			struct task_struct *tsk = threads[idx * ncpus + n];
			int err;

			if (!tsk)
				continue;

			err = kthread_stop(tsk);
			if (err < 0 && !ret)
				ret = err;

			put_task_struct(tsk);
		}

		num_waits += atomic_long_read(&smoke[idx].num_waits);
		num_fences += atomic_long_read(&smoke[idx].num_fences);
		idx++;
	}
	pr_info("Completed %lu waits for %lu fences across %d engines and %d cpus\n",
		num_waits, num_fences, RUNTIME_INFO(i915)->num_engines, ncpus);

	ret = igt_live_test_end(&live) ?: ret;
out_contexts:
	kfree(smoke[0].contexts);
out_threads:
	kfree(threads);
out_smoke:
	kfree(smoke);
out_file:
	fput(file);
out_rpm:
	intel_runtime_pm_put(&i915->runtime_pm, wakeref);

	return ret;
}

int i915_request_live_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(live_nop_request),
		SUBTEST(live_all_engines),
		SUBTEST(live_sequential_engines),
		SUBTEST(live_parallel_engines),
		SUBTEST(live_empty_request),
		SUBTEST(live_breadcrumbs_smoketest),
	};

	if (intel_gt_is_wedged(&i915->gt))
		return 0;

	return i915_subtests(tests, i915);
}

static int switch_to_kernel_sync(struct intel_context *ce, int err)
{
	struct i915_request *rq;
	struct dma_fence *fence;

	rq = intel_engine_create_kernel_request(ce->engine);
	if (IS_ERR(rq))
		return PTR_ERR(rq);

	fence = i915_active_fence_get(&ce->timeline->last_request);
	if (fence) {
		i915_request_await_dma_fence(rq, fence);
		dma_fence_put(fence);
	}

	rq = i915_request_get(rq);
	i915_request_add(rq);
	if (i915_request_wait(rq, 0, HZ / 2) < 0 && !err)
		err = -ETIME;
	i915_request_put(rq);

	while (!err && !intel_engine_is_idle(ce->engine))
		intel_engine_flush_submission(ce->engine);

	return err;
}

struct perf_stats {
	struct intel_engine_cs *engine;
	unsigned long count;
	ktime_t time;
	ktime_t busy;
	u64 runtime;
};

struct perf_series {
	struct drm_i915_private *i915;
	unsigned int nengines;
	struct intel_context *ce[];
};

static int s_sync0(void *arg)
{
	struct perf_series *ps = arg;
	IGT_TIMEOUT(end_time);
	unsigned int idx = 0;
	int err = 0;

	GEM_BUG_ON(!ps->nengines);
	do {
		struct i915_request *rq;

		rq = i915_request_create(ps->ce[idx]);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			break;
		}

		i915_request_get(rq);
		i915_request_add(rq);

		if (i915_request_wait(rq, 0, HZ / 5) < 0)
			err = -ETIME;
		i915_request_put(rq);
		if (err)
			break;

		if (++idx == ps->nengines)
			idx = 0;
	} while (!__igt_timeout(end_time, NULL));

	return err;
}

static int s_sync1(void *arg)
{
	struct perf_series *ps = arg;
	struct i915_request *prev = NULL;
	IGT_TIMEOUT(end_time);
	unsigned int idx = 0;
	int err = 0;

	GEM_BUG_ON(!ps->nengines);
	do {
		struct i915_request *rq;

		rq = i915_request_create(ps->ce[idx]);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			break;
		}

		i915_request_get(rq);
		i915_request_add(rq);

		if (prev && i915_request_wait(prev, 0, HZ / 5) < 0)
			err = -ETIME;
		i915_request_put(prev);
		prev = rq;
		if (err)
			break;

		if (++idx == ps->nengines)
			idx = 0;
	} while (!__igt_timeout(end_time, NULL));
	i915_request_put(prev);

	return err;
}

static int s_many(void *arg)
{
	struct perf_series *ps = arg;
	IGT_TIMEOUT(end_time);
	unsigned int idx = 0;

	GEM_BUG_ON(!ps->nengines);
	do {
		struct i915_request *rq;

		rq = i915_request_create(ps->ce[idx]);
		if (IS_ERR(rq))
			return PTR_ERR(rq);

		i915_request_add(rq);

		if (++idx == ps->nengines)
			idx = 0;
	} while (!__igt_timeout(end_time, NULL));

	return 0;
}

static int perf_series_engines(void *arg)
{
	struct drm_i915_private *i915 = arg;
	static int (* const func[])(void *arg) = {
		s_sync0,
		s_sync1,
		s_many,
		NULL,
	};
	const unsigned int nengines = num_uabi_engines(i915);
	struct intel_engine_cs *engine;
	int (* const *fn)(void *arg);
	struct pm_qos_request qos;
	struct perf_stats *stats;
	struct perf_series *ps;
	unsigned int idx;
	int err = 0;

	stats = kcalloc(nengines, sizeof(*stats), GFP_KERNEL);
	if (!stats)
		return -ENOMEM;

	ps = kzalloc(struct_size(ps, ce, nengines), GFP_KERNEL);
	if (!ps) {
		kfree(stats);
		return -ENOMEM;
	}

	cpu_latency_qos_add_request(&qos, 0); /* disable cstates */

	ps->i915 = i915;
	ps->nengines = nengines;

	idx = 0;
	for_each_uabi_engine(engine, i915) {
		struct intel_context *ce;

		ce = intel_context_create(engine);
		if (IS_ERR(ce))
			goto out;

		err = intel_context_pin(ce);
		if (err) {
			intel_context_put(ce);
			goto out;
		}

		ps->ce[idx++] = ce;
	}
	GEM_BUG_ON(idx != ps->nengines);

	for (fn = func; *fn && !err; fn++) {
		char name[KSYM_NAME_LEN];
		struct igt_live_test t;

		snprintf(name, sizeof(name), "%ps", *fn);
		err = igt_live_test_begin(&t, i915, __func__, name);
		if (err)
			break;

		for (idx = 0; idx < nengines; idx++) {
			struct perf_stats *p =
				memset(&stats[idx], 0, sizeof(stats[idx]));
			struct intel_context *ce = ps->ce[idx];

			p->engine = ps->ce[idx]->engine;
			intel_engine_pm_get(p->engine);

			if (intel_engine_supports_stats(p->engine))
				p->busy = intel_engine_get_busy_time(p->engine) + 1;
			p->runtime = -intel_context_get_total_runtime_ns(ce);
			p->time = ktime_get();
		}

		err = (*fn)(ps);
		if (igt_live_test_end(&t))
			err = -EIO;

		for (idx = 0; idx < nengines; idx++) {
			struct perf_stats *p = &stats[idx];
			struct intel_context *ce = ps->ce[idx];
			int integer, decimal;
			u64 busy, dt;

			p->time = ktime_sub(ktime_get(), p->time);
			if (p->busy) {
				p->busy = ktime_sub(intel_engine_get_busy_time(p->engine),
						    p->busy - 1);
			}

			err = switch_to_kernel_sync(ce, err);
			p->runtime += intel_context_get_total_runtime_ns(ce);
			intel_engine_pm_put(p->engine);

			busy = 100 * ktime_to_ns(p->busy);
			dt = ktime_to_ns(p->time);
			if (dt) {
				integer = div64_u64(busy, dt);
				busy -= integer * dt;
				decimal = div64_u64(100 * busy, dt);
			} else {
				integer = 0;
				decimal = 0;
			}

			pr_info("%s %5s: { seqno:%d, busy:%d.%02d%%, runtime:%lldms, walltime:%lldms }\n",
				name, p->engine->name, ce->timeline->seqno,
				integer, decimal,
				div_u64(p->runtime, 1000 * 1000),
				div_u64(ktime_to_ns(p->time), 1000 * 1000));
		}
	}

out:
	for (idx = 0; idx < nengines; idx++) {
		if (IS_ERR_OR_NULL(ps->ce[idx]))
			break;

		intel_context_unpin(ps->ce[idx]);
		intel_context_put(ps->ce[idx]);
	}
	kfree(ps);

	cpu_latency_qos_remove_request(&qos);
	kfree(stats);
	return err;
}

static int p_sync0(void *arg)
{
	struct perf_stats *p = arg;
	struct intel_engine_cs *engine = p->engine;
	struct intel_context *ce;
	IGT_TIMEOUT(end_time);
	unsigned long count;
	bool busy;
	int err = 0;

	ce = intel_context_create(engine);
	if (IS_ERR(ce))
		return PTR_ERR(ce);

	err = intel_context_pin(ce);
	if (err) {
		intel_context_put(ce);
		return err;
	}

	busy = false;
	if (intel_engine_supports_stats(engine)) {
		p->busy = intel_engine_get_busy_time(engine);
		busy = true;
	}

	p->time = ktime_get();
	count = 0;
	do {
		struct i915_request *rq;

		rq = i915_request_create(ce);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			break;
		}

		i915_request_get(rq);
		i915_request_add(rq);

		err = 0;
		if (i915_request_wait(rq, 0, HZ / 5) < 0)
			err = -ETIME;
		i915_request_put(rq);
		if (err)
			break;

		count++;
	} while (!__igt_timeout(end_time, NULL));
	p->time = ktime_sub(ktime_get(), p->time);

	if (busy) {
		p->busy = ktime_sub(intel_engine_get_busy_time(engine),
				    p->busy);
	}

	err = switch_to_kernel_sync(ce, err);
	p->runtime = intel_context_get_total_runtime_ns(ce);
	p->count = count;

	intel_context_unpin(ce);
	intel_context_put(ce);
	return err;
}

static int p_sync1(void *arg)
{
	struct perf_stats *p = arg;
	struct intel_engine_cs *engine = p->engine;
	struct i915_request *prev = NULL;
	struct intel_context *ce;
	IGT_TIMEOUT(end_time);
	unsigned long count;
	bool busy;
	int err = 0;

	ce = intel_context_create(engine);
	if (IS_ERR(ce))
		return PTR_ERR(ce);

	err = intel_context_pin(ce);
	if (err) {
		intel_context_put(ce);
		return err;
	}

	busy = false;
	if (intel_engine_supports_stats(engine)) {
		p->busy = intel_engine_get_busy_time(engine);
		busy = true;
	}

	p->time = ktime_get();
	count = 0;
	do {
		struct i915_request *rq;

		rq = i915_request_create(ce);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			break;
		}

		i915_request_get(rq);
		i915_request_add(rq);

		err = 0;
		if (prev && i915_request_wait(prev, 0, HZ / 5) < 0)
			err = -ETIME;
		i915_request_put(prev);
		prev = rq;
		if (err)
			break;

		count++;
	} while (!__igt_timeout(end_time, NULL));
	i915_request_put(prev);
	p->time = ktime_sub(ktime_get(), p->time);

	if (busy) {
		p->busy = ktime_sub(intel_engine_get_busy_time(engine),
				    p->busy);
	}

	err = switch_to_kernel_sync(ce, err);
	p->runtime = intel_context_get_total_runtime_ns(ce);
	p->count = count;

	intel_context_unpin(ce);
	intel_context_put(ce);
	return err;
}

static int p_many(void *arg)
{
	struct perf_stats *p = arg;
	struct intel_engine_cs *engine = p->engine;
	struct intel_context *ce;
	IGT_TIMEOUT(end_time);
	unsigned long count;
	int err = 0;
	bool busy;

	ce = intel_context_create(engine);
	if (IS_ERR(ce))
		return PTR_ERR(ce);

	err = intel_context_pin(ce);
	if (err) {
		intel_context_put(ce);
		return err;
	}

	busy = false;
	if (intel_engine_supports_stats(engine)) {
		p->busy = intel_engine_get_busy_time(engine);
		busy = true;
	}

	count = 0;
	p->time = ktime_get();
	do {
		struct i915_request *rq;

		rq = i915_request_create(ce);
		if (IS_ERR(rq)) {
			err = PTR_ERR(rq);
			break;
		}

		i915_request_add(rq);
		count++;
	} while (!__igt_timeout(end_time, NULL));
	p->time = ktime_sub(ktime_get(), p->time);

	if (busy) {
		p->busy = ktime_sub(intel_engine_get_busy_time(engine),
				    p->busy);
	}

	err = switch_to_kernel_sync(ce, err);
	p->runtime = intel_context_get_total_runtime_ns(ce);
	p->count = count;

	intel_context_unpin(ce);
	intel_context_put(ce);
	return err;
}

static int perf_parallel_engines(void *arg)
{
	struct drm_i915_private *i915 = arg;
	static int (* const func[])(void *arg) = {
		p_sync0,
		p_sync1,
		p_many,
		NULL,
	};
	const unsigned int nengines = num_uabi_engines(i915);
	struct intel_engine_cs *engine;
	int (* const *fn)(void *arg);
	struct pm_qos_request qos;
	struct {
		struct perf_stats p;
		struct task_struct *tsk;
	} *engines;
	int err = 0;

	engines = kcalloc(nengines, sizeof(*engines), GFP_KERNEL);
	if (!engines)
		return -ENOMEM;

	cpu_latency_qos_add_request(&qos, 0);

	for (fn = func; *fn; fn++) {
		char name[KSYM_NAME_LEN];
		struct igt_live_test t;
		unsigned int idx;

		snprintf(name, sizeof(name), "%ps", *fn);
		err = igt_live_test_begin(&t, i915, __func__, name);
		if (err)
			break;

		atomic_set(&i915->selftest.counter, nengines);

		idx = 0;
		for_each_uabi_engine(engine, i915) {
			intel_engine_pm_get(engine);

			memset(&engines[idx].p, 0, sizeof(engines[idx].p));
			engines[idx].p.engine = engine;

			engines[idx].tsk = kthread_run(*fn, &engines[idx].p,
						       "igt:%s", engine->name);
			if (IS_ERR(engines[idx].tsk)) {
				err = PTR_ERR(engines[idx].tsk);
				intel_engine_pm_put(engine);
				break;
			}
			get_task_struct(engines[idx++].tsk);
		}

		yield(); /* start all threads before we kthread_stop() */

		idx = 0;
		for_each_uabi_engine(engine, i915) {
			int status;

			if (IS_ERR(engines[idx].tsk))
				break;

			status = kthread_stop(engines[idx].tsk);
			if (status && !err)
				err = status;

			intel_engine_pm_put(engine);
			put_task_struct(engines[idx++].tsk);
		}

		if (igt_live_test_end(&t))
			err = -EIO;
		if (err)
			break;

		idx = 0;
		for_each_uabi_engine(engine, i915) {
			struct perf_stats *p = &engines[idx].p;
			u64 busy = 100 * ktime_to_ns(p->busy);
			u64 dt = ktime_to_ns(p->time);
			int integer, decimal;

			if (dt) {
				integer = div64_u64(busy, dt);
				busy -= integer * dt;
				decimal = div64_u64(100 * busy, dt);
			} else {
				integer = 0;
				decimal = 0;
			}

			GEM_BUG_ON(engine != p->engine);
			pr_info("%s %5s: { count:%lu, busy:%d.%02d%%, runtime:%lldms, walltime:%lldms }\n",
				name, engine->name, p->count, integer, decimal,
				div_u64(p->runtime, 1000 * 1000),
				div_u64(ktime_to_ns(p->time), 1000 * 1000));
			idx++;
		}
	}

	cpu_latency_qos_remove_request(&qos);
	kfree(engines);
	return err;
}

int i915_request_perf_selftests(struct drm_i915_private *i915)
{
	static const struct i915_subtest tests[] = {
		SUBTEST(perf_series_engines),
		SUBTEST(perf_parallel_engines),
	};

	if (intel_gt_is_wedged(&i915->gt))
		return 0;

	return i915_subtests(tests, i915);
}
