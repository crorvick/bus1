/*
 * Copyright (C) 2013-2016 Red Hat, Inc.
 *
 * bus1 is free software; you can redistribute it and/or modify it under
 * the terms of the GNU Lesser General Public License as published by the
 * Free Software Foundation; either version 2.1 of the License, or (at
 * your option) any later version.
 */

#define pr_fmt(fmt) KBUILD_MODNAME ": " fmt
#include <linux/err.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/uaccess.h>
#include <uapi/linux/bus1.h>
#include "domain.h"
#include "filesystem.h"
#include "peer.h"
#include "pool.h"
#include "queue.h"
#include "transaction.h"
#include "util.h"

/**
 * bus1_peer_new() - create new peer
 * @param:	parameter for peer
 *
 * Allocate a new peer object with the given parameters. The peer is not linked
 * into any domain, nor is any locking required for this call.
 *
 * Return: Pointer to new peer, or ERR_PTR on failure.
 */
struct bus1_peer *bus1_peer_new(struct bus1_cmd_connect *param)
{
	struct bus1_peer *peer;
	int r;

	if (unlikely(param->pool_size == 0 ||
		     !IS_ALIGNED(param->pool_size, PAGE_SIZE)))
		return ERR_PTR(-EINVAL);

	peer = kmalloc(sizeof(*peer), GFP_KERNEL);
	if (!peer)
		return ERR_PTR(-ENOMEM);

	mutex_init(&peer->lock);
	peer->pool = BUS1_POOL_NULL;
	bus1_queue_init_for_peer(&peer->queue, peer);

	r = bus1_pool_create(&peer->pool, param->pool_size);
	if (r < 0)
		goto error;

	return peer;

error:
	bus1_peer_free(peer);
	return ERR_PTR(r);
}

/**
 * bus1_peer_free() - destroy peer
 * @peer:	peer to destroy, or NULL
 *
 * This destroys and deallocates a peer object, which was previously created
 * via bus1_peer_new(). The caller must make sure no-one else is accessing the
 * peer object at the same time.
 *
 * The peer-object is released in an rcu-delayed manner. That is, the object
 * will stay accessible for at least one rcu grace period.
 *
 * If NULL is passed, this is a no-op.
 *
 * Return: NULL is returned.
 */
struct bus1_peer *bus1_peer_free(struct bus1_peer *peer)
{
	if (!peer)
		return NULL;

	bus1_queue_destroy(&peer->queue);
	bus1_pool_destroy(&peer->pool);

	/*
	 * Make sure the peer object is freed in a delayed-manner. Some
	 * embedded members (like the queue) must be accessible for an entire
	 * rcu read-side critical section.
	 */
	kfree_rcu(peer, rcu);

	return NULL;
}

/**
 * bus1_peer_reset() - reset peer
 * @peer:	peer to reset
 * @id:		ID of peer
 *
 * Reset a peer object. The caller must provide the new peer ID as @id. This
 * function will flush all data on the peer, which is tagged with an ID that
 * does not match the new ID @id.
 *
 * No locking is required by the caller. However, the caller obviously must
 * make sure they own the object.
 */
void bus1_peer_reset(struct bus1_peer *peer, u64 id)
{
	mutex_lock(&peer->lock);
	/* XXX: flush outdated queue entries */
	mutex_unlock(&peer->lock);
}

static int bus1_peer_send(struct bus1_peer *peer,
			  u64 peer_id,
			  struct bus1_fs_domain *fs_domain,
			  struct bus1_domain *domain,
			  unsigned long arg,
			  bool is_compat)
{
	struct bus1_transaction *transaction = NULL;
	const u64 __user *ptr_dest;
	struct bus1_cmd_send param;
	u64 destination;
	size_t i;
	int r;

	r = bus1_import_fixed_ioctl(&param, arg, sizeof(param));
	if (r < 0)
		return r;

	if (unlikely(param.flags & ~(BUS1_SEND_FLAG_IGNORE_UNKNOWN |
				     BUS1_SEND_FLAG_CONVEY_ERRORS)))
		return -EINVAL;

	/* check basic limits; avoids integer-overflows later on */
	if (unlikely(param.n_destinations > BUS1_DESTINATION_MAX) ||
	    unlikely(param.n_vecs > BUS1_VEC_MAX) ||
	    unlikely(param.n_fds > BUS1_FD_MAX))
		return -EMSGSIZE;

	/* 32bit pointer validity checks */
	if (unlikely(param.ptr_destinations !=
		     (u64)(unsigned long)param.ptr_destinations) ||
	    unlikely(param.ptr_vecs !=
		     (u64)(unsigned long)param.ptr_vecs) ||
	    unlikely(param.ptr_fds !=
		     (u64)(unsigned long)param.ptr_fds))
		return -EFAULT;

	transaction = bus1_transaction_new_from_user(fs_domain, domain,
						     peer_id, &param,
						     is_compat);
	if (IS_ERR(transaction))
		return PTR_ERR(transaction);

	ptr_dest = (const u64 __user *)(unsigned long)param.ptr_destinations;
	for (i = 0; i < param.n_destinations; ++i) {
		/* faults are always fatal for any transaction */
		if (get_user(destination, ptr_dest + i)) {
			r = -EFAULT;
			goto exit;
		}

		r = bus1_transaction_instantiate_for_id(transaction,
							destination,
							param.flags);
		if (r < 0)
			goto exit;
	}

	bus1_transaction_commit(transaction);
	r = 0;

exit:
	bus1_transaction_free(transaction);
	return r;
}

static int bus1_peer_recv(struct bus1_peer *peer,
			  u64 peer_id,
			  unsigned long arg)
{
	struct bus1_cmd_recv __user *uparam = (void __user *)arg;
	struct bus1_queue_entry *entry;
	struct bus1_cmd_recv param;
	size_t wanted_fds, n_fds = 0;
	int r, *t, *fds = NULL;
	struct kvec vec;

	r = bus1_import_fixed_ioctl(&param, arg, sizeof(param));
	if (r < 0)
		return r;

	if (unlikely(param.flags & ~(BUS1_RECV_FLAG_PEEK)))
		return -EINVAL;

	if (unlikely(param.msg_offset != 0) ||
	    unlikely(param.msg_size != 0) ||
	    unlikely(param.msg_fds != 0))
		return -EINVAL;

	/*
	 * Peek at the first message to fetch the FD count. We need to
	 * pre-allocate FDs, to avoid dropping messages due to FD exhaustion.
	 * If no entry is queued, we can bail out early.
	 * Note that this is just a fast-path optimization. Anyone might race
	 * us for message retrieval, so we have to check it again below.
	 */
	rcu_read_lock();
	entry = bus1_queue_peek_rcu(&peer->queue);
	wanted_fds = entry ? entry->n_files : 0;
	rcu_read_unlock();
	if (!entry)
		return -EAGAIN;

	/*
	 * Deal with PEEK first. This is simple. Just look at the first queued
	 * message, publish the slice and return the information to user-space.
	 * Keep the entry queued, so it can be peeked multiple times, and
	 * received later on.
	 * We do not install any FDs for PEEK, but provide the number in
	 * msg_fds, anyway.
	 */
	if (param.flags & BUS1_RECV_FLAG_PEEK) {
		mutex_lock(&peer->lock);
		entry = bus1_queue_peek(&peer->queue);
		if (entry) {
			bus1_pool_publish(&peer->pool, entry->slice,
					  &param.msg_offset, &param.msg_size);
			param.msg_fds = entry->n_files;
		}
		mutex_unlock(&peer->lock);

		if (!entry)
			return -EAGAIN;

		r = 0;
		goto exit;
	}

	/*
	 * So there is a message queued with 'wanted_fds' attached FDs.
	 * Allocate a temporary buffer to store them, then dequeue the message.
	 * In case someone raced us and the message changed, re-allocate the
	 * temporary buffer and retry.
	 */

	do {
		if (wanted_fds > n_fds) {
			t = krealloc(fds, wanted_fds * sizeof(*fds),
				     GFP_TEMPORARY);
			if (!t) {
				r = -ENOMEM;
				goto exit;
			}

			fds = t;
			for ( ; n_fds < wanted_fds; ++n_fds) {
				r = get_unused_fd_flags(O_CLOEXEC);
				if (r < 0)
					goto exit;

				fds[n_fds] = r;
			}
		}

		mutex_lock(&peer->lock);
		entry = bus1_queue_peek(&peer->queue);
		if (!entry) {
			/* nothing to do, caught below */
		} else if (entry->n_files > n_fds) {
			/* re-allocate FD array and retry */
			wanted_fds = entry->n_files;
		} else {
			bus1_queue_unlink(&peer->queue, entry);
			bus1_pool_publish(&peer->pool, entry->slice,
					  &param.msg_offset, &param.msg_size);
			param.msg_fds = entry->n_files;

			/*
			 * Fastpath: If no FD is transmitted, we can avoid the
			 *           second lock below. Directly release the
			 *           slice.
			 */
			if (entry->n_files == 0)
				bus1_pool_release_kernel(&peer->pool,
							 entry->slice);
		}
		mutex_unlock(&peer->lock);
	} while (wanted_fds > n_fds);

	if (!entry) {
		r = -EAGAIN;
		goto exit;
	}

	while (n_fds > entry->n_files)
		put_unused_fd(fds[--n_fds]);

	if (n_fds > 0) {
		/*
		 * We dequeued the message, we already fetched enough FDs, all
		 * we have to do is copy the FD numbers into the slice and link
		 * the FDs.
		 * The only reason this can fail, is if writing the pool fails,
		 * which itself can only happen during OOM. In that case, we
		 * don't support reverting the operation, but you rather lose
		 * the message. We cannot put it back on the queue (would break
		 * ordering), and we don't want to perform the copy-operation
		 * while holding the queue-lock.
		 * We treat this OOM as if the actual message transaction OOMed
		 * and simply drop the message.
		 */

		vec.iov_base = fds;
		vec.iov_len = n_fds * sizeof(*fds);

		r = bus1_pool_write_kvec(&peer->pool, entry->slice,
					 entry->slice->size - vec.iov_len,
					 &vec, 1, vec.iov_len);

		mutex_lock(&peer->lock);
		bus1_pool_release_kernel(&peer->pool, entry->slice);
		mutex_unlock(&peer->lock);

		/* on success, install FDs; on error, see fput() in `exit:' */
		if (r >= 0) {
			for ( ; n_fds > 0; --n_fds)
				fd_install(fds[n_fds - 1],
					   get_file(entry->files[n_fds - 1]));
		} else {
			/* XXX: convey error, just like in transactions */
		}
	} else {
		/* slice is already released, nothing to do */
		r = 0;
	}

	entry->slice = NULL;
	bus1_queue_entry_free(entry);

exit:
	if (r >= 0) {
		if (put_user(param.msg_offset, &uparam->msg_offset) ||
		    put_user(param.msg_size, &uparam->msg_size) ||
		    put_user(param.msg_fds, &uparam->msg_fds))
			r = -EFAULT; /* Don't care.. keep what we did so far */
	}
	while (n_fds > 0)
		put_unused_fd(fds[--n_fds]);
	kfree(fds);
	return r;
}

/**
 * bus1_peer_ioctl() - handle peer ioctl
 * @peer:		peer to work on
 * @peer_id:		current ID of this peer
 * @fs_domain:		parent domain handle
 * @domain:		parent domain
 * @cmd:		ioctl command
 * @arg:		ioctl argument
 * @is_compat:		compat ioctl
 *
 * This handles the given ioctl (cmd+arg) on the passed peer @peer. The caller
 * must make sure the peer is pinned, its current ID is provided as @peer_id,
 * its parent domain handle is pinned as @fs_domain, and dereferenced as
 * @domain.
 *
 * Multiple ioctls can be called in parallel just fine. No locking is needed.
 *
 * Return: 0 on success, negative error code on failure.
 */
int bus1_peer_ioctl(struct bus1_peer *peer,
		    u64 peer_id,
		    struct bus1_fs_domain *fs_domain,
		    struct bus1_domain *domain,
		    unsigned int cmd,
		    unsigned long arg,
		    bool is_compat)
{
	int r;

	switch (cmd) {
	case BUS1_CMD_FREE:
		r = 0; /* XXX */
		break;
	case BUS1_CMD_TRACK:
		r = 0; /* XXX */
		break;
	case BUS1_CMD_UNTRACK:
		r = 0; /* XXX */
		break;
	case BUS1_CMD_SEND:
		r = bus1_peer_send(peer, peer_id, fs_domain, domain,
				   arg, is_compat);
		break;
	case BUS1_CMD_RECV:
		r = bus1_peer_recv(peer, peer_id, arg);
		break;
	default:
		r = -ENOTTY;
		break;
	}

	return r;
}
