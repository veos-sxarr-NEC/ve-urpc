#include <stdarg.h>

#include "urpc_common.h"
#include "ve_inst.h"
#include "urpc_time.h"
#ifdef __ve__
#include <vedma.h>
#else
#include "vh_shm.h"
#endif

/*
  Wait for the shared memory segment to be attached by two processes.
  When done, mark the shared memory segment as destroyed.
  This avoids left-over shared memory segments in the VH memory.
*/
int wait_peer_attach(urpc_peer_t *up)
{
#ifndef __ve__
	return vh_shm_wait_peers(up->shm_segid);
#endif
}

/*
  Free payload blocks of finished requests and adjust free block pointers
 */
static uint32_t _gc_buffer(urpc_comm_t *uc)
{
	uint32_t last_req = TQ_READ64(uc->tq->last_put_req);
	uint32_t last_slot = REQ2SLOT(last_req);
	TQ_FENCE();
	// if we're at the end of the buffer, assign the tiny rest to the
	// last sent request
	if (uc->free_end == URPC_DATA_BUFF_LEN) {
		mlist_t *ml = &uc->mlist[last_slot];
		if (ml->b.len == 0)
			ml->b.offs = uc->free_begin;
		ml->b.len = uc->free_end - ml->b.offs;
		uc->free_begin = uc->free_end = 0;
		dprintf("gc: free_begin=%u free_end=%u\n", uc->free_begin, uc->free_end);
	}
	//
	// loop through req slots and free the ones which are finished
	//
	for (int i = 1; i <= URPC_LEN_MB; i++) {
		int slot = (last_slot + i) % URPC_LEN_MB;
		mlist_t *ml = &uc->mlist[slot];
		urpc_mb_t m;
		m.u64 = TQ_READ64(uc->tq->mb[slot].u64);
		TQ_FENCE();
		if (m.c.cmd == URPC_CMD_NONE && ml->b.len > 0) {
			if (uc->free_end < URPC_DATA_BUFF_LEN)
				uc->free_end = ALIGN8B(ml->b.offs + ml->b.len);
			ml->u64 = 0;
			TQ_WRITE64(uc->tq->mb[slot].u64, 0);
		}
	}
	dprintf("gc: free_begin=%u free_end=%u DBL=%u\n",
		uc->free_begin, uc->free_end, URPC_DATA_BUFF_LEN);
	return uc->free_end - uc->free_begin;
}
	
/*
  Allocate a payload buffer.

  Returns 0 if allocation failed, otherwise a urpc_mb_t with empty command field
  but filled offs and len fields.
 */
static uint64_t _alloc_payload(urpc_comm_t *uc, uint32_t size)
{
	urpc_mb_t res;
	uint32_t asize = ALIGN8B(size);
        long ts = get_time_us();

	res.u64 = 0;
	while (uc->free_end - uc->free_begin < asize) {
		uint32_t new_free = _gc_buffer(uc);
		if (new_free < size) {
			// TODO: delay, count, timeout
			if (timediff_us(ts) > URPC_ALLOC_TIMEOUT_US) {
				eprintf("ERROR: alloc_payload timed out!\n");
				return 0;
			}
		} else {
			break;
                }
	}
	if (uc->free_begin + asize > uc->free_end) {
		printf("alloc: free_begin=%u free_end=%u asize=%u\n",
		       uc->free_begin, uc->free_end, asize);
		return 0;
	}
		
	res.c.offs = uc->free_begin;
	uc->free_begin += ALIGN8B(size);
	res.c.len = size;

	return res.u64;
}

uint32_t urpc_get_receiver_flags(urpc_comm_t *uc)
{
	return TQ_READ32(uc->tq->receiver_flags);

}

void urpc_set_receiver_flags(urpc_comm_t *uc, uint32_t flags)
{
	TQ_WRITE32(uc->tq->receiver_flags, flags);
}

uint32_t urpc_get_sender_flags(urpc_comm_t *uc)
{
	return TQ_READ32(uc->tq->sender_flags);

}

void urpc_set_sender_flags(urpc_comm_t *uc, uint32_t flags)
{
	TQ_WRITE32(uc->tq->sender_flags, flags);
}

/*
  Pull next command from the transfer queue.

  Returns: req ID for cmd or -1
 */
int64_t urpc_get_cmd(transfer_queue_t *tq, urpc_mb_t *m)
{
	int slot;
        int64_t req = -1;
	int64_t last_put = TQ_READ64(tq->last_put_req);
	int64_t last_get = TQ_READ64(tq->last_get_req);

	TQ_FENCE();
	if (last_put != last_get) {
		req = last_get + 1;
		slot = REQ2SLOT(req);
		m->u64 = TQ_READ64(tq->mb[slot].u64);
		dprintf("urpc_get_cmd req=%ld cmd=%u offs=%u len=%u\n",
                        req, m->c.cmd, m->c.offs, m->c.len);
		TQ_WRITE64(tq->last_get_req, req);
		TQ_FENCE();
	}
	return req;
}

/*
  Wait for a request, with timeout.

  Return request ID if ok, -1 if failed.
*/
int64_t urpc_get_cmd_timeout(transfer_queue_t *tq, urpc_mb_t *m, long timeout_us)
{
	int64_t res;

	long done_ts = get_time_us();

	while (((res = urpc_get_cmd(tq, m)) == -1) &&
	       timediff_us(done_ts) < timeout_us);
	return res;
}

/*
  Pull a certain command from the transfer queue, specified by req.

  Returns: req if successful or -1 if not.
 */
int64_t urpc_get_req(transfer_queue_t *tq, urpc_mb_t *m, int64_t req)
{
	int slot;
	int64_t last_put = TQ_READ64(tq->last_put_req);
	int64_t last_get = TQ_READ64(tq->last_get_req);

	if (last_get >= req) {
		dprintf("urpc_get_req: req %ld already handled!?", req);
		return -1;
	}

	TQ_FENCE();
	if (last_put >= req) {
		slot = REQ2SLOT(req);
		m->u64 = TQ_READ64(tq->mb[slot].u64);
		dprintf("urpc_get_req req=%ld cmd=%u offs=%u len=%u\n",
                        req, m->c.cmd, m->c.offs, m->c.len);
		if (last_get + 1 == req) {	
			TQ_WRITE64(tq->last_get_req, req);
			TQ_FENCE();
		}
                return req;
	}
	return -1;
}

/*
  Erase command field in a slot, marking this command as done.

  This is being done on the receiver side of a communicator.
  Associated payload storage is managed on the sender side.

  Returns: slot for cmd or -1
 */
void urpc_slot_done(transfer_queue_t *tq, int slot, urpc_mb_t *m)
{
	m->c.cmd = URPC_CMD_NONE;
        TQ_FENCE();
	TQ_WRITE64(tq->mb[slot].u64, m->u64);
        TQ_FENCE();
}

/*
  Write mlist entry for chosen slot.
*/


/*
  Put a command in the next mailbox slot.

  Wait if the slot is busy.

  Return request_number.
 */
int64_t urpc_put_cmd(urpc_peer_t *up, urpc_mb_t *m)
{
	urpc_comm_t *uc = &up->send;
	transfer_queue_t *tq = uc->tq;
	int slot = -1;
	urpc_mb_t next;
	int64_t req = TQ_READ64(tq->last_put_req) + 1;

	TQ_FENCE();
	slot = REQ2SLOT(req);
	do {
		next.u64 = TQ_READ64(tq->mb[slot].u64);
		TQ_FENCE();
		// TODO: timeout
	} while(next.c.cmd != URPC_CMD_NONE);

	mlist_t *ml = &uc->mlist[slot];

	if (ml->b.len) {
		if (uc->free_end < URPC_DATA_BUFF_LEN) {
			if (ml->b.offs == uc->free_end)
				uc->free_end += ml->b.len;
		}
	}
	if (m->c.len) {
		ml->b.len = m->c.len;
		ml->b.offs = m->c.offs;
	} else
		ml->u64 = 0;

	TQ_WRITE64(tq->mb[slot].u64, m->u64);
	TQ_WRITE64(tq->last_put_req, req);
        dprintf("urpc_put_cmd req=%ld cmd=%u offs=%u len=%u\n",
                req, m->c.cmd, m->c.offs, m->c.len);
	return req;
}


static int set_recv_payload(urpc_comm_t *uc, urpc_mb_t *m, void **payload, size_t *plen)
{
	transfer_queue_t *tq = uc->tq;
	int err;

	//
	// Set payload pointer
	//
	if (m->c.len > 0) {
#ifdef __ve__
		*payload = (void *)((char *)uc->mirr_data_buff + m->c.offs);
		*plen = m->c.len;
		if (*plen <= 16) {
			int aoffs = m->c.offs >> 3;  // divide by 8
			for (int i = 0; i < *plen >> 3; i++) {
				((uint64_t *)(uc->mirr_data_buff))[aoffs + i] =
					TQ_READ64(tq->data[aoffs + i]);
			}
		} else {
			
			//
			// do the DMA transfer synchronously
			//
			err = ve_dma_post_wait(uc->mirr_data_vehva + m->c.offs, // dst
					       uc->shm_data_vehva + m->c.offs,  // src
					       *plen);
			if (err) {
				eprintf("[VE ERROR] ve_dma_post_wait failed: %x\n", err);
				return -EIO;
			}
		}
#else
		*payload = (void *)((char *)&tq->data[0] + m->c.offs);
		*plen = m->c.len;
#endif

	} else {
		*payload = NULL;
		*plen = 0;
	}
	return 0;
}

/*
  Wait for a particular request, with timeout.

  Return true if request found, false otherwise.
*/
int urpc_recv_req_timeout(urpc_peer_t *up, urpc_mb_t *m, int64_t req, long timeout_us,
			  void **payload, size_t *plen)
{
	int64_t res;
        urpc_comm_t *uc = &up->recv;
	transfer_queue_t *tq = uc->tq;

	long done_ts = get_time_us();

	while (((res = urpc_get_req(tq, m, req)) == -1) &&
	       timediff_us(done_ts) < timeout_us);
	if (res == req) {
		//
		// set/receive payload, if needed
		//
		set_recv_payload(uc, m, payload, plen);
	}		
	return res == req;
}

/*
  URPC progress function.

  Process at most 'ncmds' requests from the RECV communicator.
  Return number of requests processed.
*/
int urpc_recv_progress(urpc_peer_t *up, int ncmds)
{
	urpc_comm_t *uc = &up->recv;
	transfer_queue_t *tq = uc->tq;
	urpc_handler_func func = NULL;
	int err = 0, done = 0;
	urpc_mb_t m;
	void *payload = NULL;
	size_t plen = 0;

	// TODO: aggregate contiguous payload buffers
	while (done < ncmds) {
		int64_t req = urpc_get_cmd(tq, &m);
		if (req < 0)
			break;
		//
		// set/receive payload, if needed
		//
		set_recv_payload(uc, &m, &payload, &plen);
		//
		// call handler
		//
		func = up->handler[m.c.cmd];
		if (func) {
			err = func(up, &m, req, payload, plen);
			if (err)
				eprintf("Warning: RPC handler %d returned %d\n",
					m.c.cmd, err);
		}

		urpc_slot_done(tq, REQ2SLOT(req), &m);
		++done;
	}
	return done;
}

/*
  Progress loop with timeout.
*/
int urpc_recv_progress_timeout(urpc_peer_t *up, int ncmds, long timeout_us)
{
	long done_ts = 0;
	do {
		int done = urpc_recv_progress(up, ncmds);
		if (done == 0) {
			if (done_ts == 0)
				done_ts = get_time_us();
		} else
			done_ts = 0;

	} while (done_ts == 0 || timediff_us(done_ts) < timeout_us);

}

/*
  Register a RPC handler.

  Returns cmd ID if successful, a negative number if not.
*/
int urpc_register_handler(urpc_peer_t *up, int cmd, urpc_handler_func handler)
{
	if (cmd <= 0 || cmd > URPC_MAX_HANDLERS)
		return -EINVAL;
	if (up->handler[cmd])
		return -EEXIST;
	up->handler[cmd] = handler;
	return cmd;
}

/*
  Unregister a RPC handler.

  Returns 0 if successful, a negative number if not.
*/
int urpc_unregister_handler(urpc_peer_t *up, int cmd)
{
	if (cmd <= 0 || cmd > URPC_MAX_HANDLERS)
		return -EINVAL;
	up->handler[cmd] = NULL;
	return 0;
}

/////////////////

/*
  Generic send command which:
  - computes the payload size
  - assembles the payload from the passed variadic arguments
  - submits the URPC command

  The payload (and variadic arguments) are described by a 'fmt' string.
  It can contain following characters:
  'I' : expect an unsigned 32 bit integer
  'L' : expect an unsigned 64 bit integer
  'x' : 32 bit padding (no argument expected)
  'P' : a buffer pointer, expects a "void *" and a "size_t" for the buffer size.
        The buffer size is packed as uint64_t into the payload and is followed by
        the buffer content.
  64 bit values and the buffer should better start at an 8 byte boundary, so use
  padding in the fmt string to achieve that. The payload length will also be
  filled to the next 8 byte boundary, such that the next payload is again 8b aligned.

  Returns the request ID.
 */
int64_t urpc_generic_send(urpc_peer_t *up, int cmd, char *fmt, ...)
{
	int rc;
	char *p, *pp, *payload;
	urpc_comm_t *uc = &up->send;
	transfer_queue_t *tq = uc->tq;
	urpc_mb_t mb = { .u64 = 0 };;

	va_list ap1, ap2;
	va_start(ap1, fmt);
	va_copy(ap2, ap1);

	// estimate size of payload
	uint32_t dummy32;
	uint64_t dummy64;
	void *dummyp;
	size_t dummys;
	size_t size = 0;
	for (p = fmt; *p != '\0'; p++) {
		switch (*p) {
		case 'I': // 32 bit value
			size += 4;
			dummy32 = va_arg(ap1, uint32_t);
			break;
		case 'L': // 64 bit value
			size += 8;
			dummy64 = va_arg(ap1, uint64_t);
			break;
		case 'P': // 64 bit value
			size += 8;
			dummyp = va_arg(ap1, void *);
			dummys = va_arg(ap1, size_t);
			size += dummys;
			break;
		case 'x': // 32 bit padding
			size += 4;
			break;
		default:
			eprintf("ERROR: illegal pack type in '%s'!\n", fmt);
			break;
		}
	}
	size = ALIGN8B(size);
	va_end(ap1);
        dprintf("generic_send allocating %ld bytes payload\n", size);
	if (size) {
		// allocate payload on data buffer
		mb.u64 = _alloc_payload(uc, (uint32_t)size);
		if (mb.u64 == 0) {
			dprintf("generic_send: failed to allocate payload\n");
			eprintf("ERROR: urpc_alloc_payload failed!");
			return -ENOMEM;
		}

		// fill payload buffer
#ifdef __ve__
		payload = (void *)((char *)uc->mirr_data_buff + mb.c.offs);
#else
		payload = (void *)((char *)&tq->data[0] + mb.c.offs);
#endif
		pp = payload;
		for (p = fmt; *p != '\0'; p++) {
			switch (*p) {
			case 'I': // 32 bit value
				*((uint32_t *)pp) = va_arg(ap2, uint32_t);
				pp += 4;
				break;
			case 'L': // 64 bit value
				*((uint64_t *)pp) = va_arg(ap2, uint64_t);
				pp += 8;
				break;
			case 'P': // 64 bit value
				dummyp = va_arg(ap2, void *);
				dummys = va_arg(ap2, size_t);
				*((uint64_t *)pp) = dummys;
				pp += 8;
                                if (dummys)
					memcpy((void *)pp, dummyp, (size_t)dummys);
				pp += dummys;
				break;
			case 'x': // 32 bit padding
				pp += 4;
				break;
			default:
				eprintf("ERROR: illegal pack type in '%s'!\n", fmt);
				break;
			}
		}
		// on VE: do the DMA/data transfer
#ifdef __ve__
		rc = ve_transfer_data_sync(uc->shm_data_vehva + mb.c.offs,
					   uc->mirr_data_vehva + mb.c.offs,
					   mb.c.len);
		if (rc) {
			eprintf("[VE ERROR] ve_dma_post_wait send failed: %x\n", rc);
			return -EIO;
		}
#endif

	}
	va_end(ap2);

	mb.c.cmd = cmd;

	// send command
	return urpc_put_cmd(up, &mb);
}

/*
  Unpack payload according to pack string. This can be used as the counterpart
  to urpc_generic_send() which does the packing.

  The payload (and variadic arguments) are described by a 'fmt' string and the
  values in the payload are unpacked. More or less. Buffer pointers just point into
  the place inside the payload. So the payload shall not ge destroyed or reused
  until the content has been used or copied away.

  The fmt string can contain following characters:
  'I' : expect an unsigned 32 bit integer. Corresponds to a "uint32_t *" in the args.
  'L' : expect an unsigned 64 bit integer. Corresponds to a "uint64_t *" in the args.
  'x' : 32 bit padding (no argument expected). Nothing in the arguments.
  'P' : a buffer pointer, expects a "void *" and a "size_t" for the buffer size.
        The buffer size is packed as uint64_t into the payload and is followed by
        the buffer content.
  64 bit values and the buffer should better start at an 8 byte boundary, so use
  padding in the fmt string to achieve that. The payload length will also be
  filled to the next 8 byte boundary, such that the next payload is again 8b aligned.

  Returns 0 if all went well, -1 if we ran out of the payload buffer.
 */
int urpc_unpack_payload(void *payload, size_t psz, char *fmt, ...)
{
	int rc = 0;
	char *p;
	char *pp = (char *)payload;
	long lsz = (long)psz;

	va_list ap;
	va_start(ap, fmt);

	// estimate size of payload
	uint32_t *dummy32;
	uint64_t *dummy64;
	void **dummyp;
	size_t *dummys;
	for (p = fmt; *p != '\0' && lsz >= 0; p++) {
		switch (*p) {
		case 'I': // 32 bit value
			dummy32 = va_arg(ap, uint32_t *);
			*dummy32 = *((uint32_t *)pp);
			pp += 4;
                        lsz -= 4;
			break;
		case 'L': // 64 bit value
			dummy64 = va_arg(ap, uint64_t *);
			*dummy64 = *((uint64_t *)pp);
			pp += 8;
                        lsz -= 8;
			break;
		case 'P': // 64 bit value
			dummyp = va_arg(ap, void **);
			dummys = va_arg(ap, size_t *);
			*dummys = (size_t) *((uint64_t *)pp);
			pp += 8;
                        lsz -= 8;
			*dummyp = (void *)pp;
			pp += *dummys;
                        lsz -= *dummys;
			break;
		case 'x': // 32 bit padding
			pp += 4;
                        lsz -= 4;
			break;
		default:
			eprintf("ERROR: illegal pack type in '%s'!\n", fmt);
			break;
		}
	}
	if (lsz < 0)
		rc = -1;
	va_end(ap);
	return rc;
}