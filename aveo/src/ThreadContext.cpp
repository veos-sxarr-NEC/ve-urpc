/**
 * @file ThreadContext.cpp
 * @brief implementation of ThreadContext
 */
#include <set>

#include <pthread.h>
#include <cerrno>
#include <semaphore.h>
#include <signal.h>

#include "veo_urpc.hpp"
#include "CallArgs.hpp"
//#include "veo_private_defs.h"
#include "ThreadContext.hpp"
#include "ProcHandle.hpp"
#include "CommandImpl.hpp"
#include "VEOException.hpp"
#include "log.hpp"

namespace veo {

ThreadContext::ThreadContext(ProcHandle *p, urpc_peer_t *up):
  proc(p), up(up), state(VEO_STATE_UNKNOWN), is_main_thread(true), seq_no(0) {}

/**
 * @brief function to be set to close request (command)
 */
int64_t ThreadContext::_closeCommandHandler(uint64_t id)
{
  VEO_TRACE(this, "%s()", __func__);
  this->state = VEO_STATE_EXIT;
  /*
   * pthread_exit() can invoke destructors for objects on the stack,
   * which can cause double-free.
   */
  auto dummy = [](Command *)->int64_t{return 0;};
  auto newc = new internal::CommandImpl(id, dummy);
  /* push the reply here because this function never returns. */
  newc->setResult(0, 0);
  this->comq.pushCompletion(std::unique_ptr<Command>(newc));
  return 0;
}

/**
 * @brief close this context
 *
 * @return zero upon success; negative upon failure.
 *
 * Close this VEO thread context; terminate the pseudo thread.
 * The current implementation always returns zero.
 */
int ThreadContext::close()
{
  if ( this->state == VEO_STATE_EXIT )
    return 0;
  // not closing a main thread, just ignoring the request
  if (this->isMainThread())
    return 0;

  // TODO: call progress until all requests done
  auto id = this->issueRequestID();
  auto f = std::bind(&ThreadContext::_closeCommandHandler, this, id);
  std::unique_ptr<Command> req(new internal::CommandImpl(id, f));
  if(this->comq.pushRequest(std::move(req)))
    id = VEO_REQUEST_ID_INVALID;
  auto c = this->comq.waitCompletion(id);
  return c->getRetval();
}

/**
 * @brief worker function for progress
 *
 *
 */
void ThreadContext::_progress_nolock(int ops)
{
  urpc_comm_t *uc = &this->up->recv;
  transfer_queue_t *tq = uc->tq;
  urpc_mb_t m;
  void *payload = nullptr;
  size_t plen;
  int recvd, sent;

  do {
    --ops;
    //
    // try to receive a command reply
    //
    recvd = sent = 0;
    int64_t req = urpc_get_cmd(tq, &m);
    if (req >= 0) {
      ++recvd;
      auto cmd = std::move(this->comq.popInFlight());
      if (cmd == nullptr) {
        // ooops!
        throw VEOException("URPC req without corresponding cmd!?", req);
      }
      set_recv_payload(uc, &m, &payload, &plen);
      //
      // call command "result function"
      //
      auto rv = (*cmd)(&m, payload, plen);
      urpc_slot_done(tq, REQ2SLOT(req), &m);
      this->comq.pushCompletion(std::move(cmd));
      if (rv < 0) {
        this->state = VEO_STATE_EXIT;
        this->comq.cancelAll();
        VEO_ERROR(nullptr, "Internal error on executing a command(%d)", rv);
        return;
      }
    }
    //
    // try to submit a new command
    //
    if (urpc_next_send_slot(this->up) < 0) {
      continue;
    }
    auto cmd = std::move(this->comq.tryPopRequest());
    if (cmd) {
      if (cmd->isVH()) {
        if (this->comq.emptyInFlight()) {
          //
          // call command "submit function"
          //
          auto rv = (*cmd)();
          this->comq.pushCompletion(std::move(cmd));
          ++sent;
        }
      } else {
        //
        // call command "submit function"
        //
        auto rv = (*cmd)();
        if (rv == 0) {
          ++sent;
          this->comq.pushInFlight(std::move(cmd));
        }
      }
    }
  } while(recvd + sent > 0); // || ops != 0);
}

/**
 * @brief Progress function for asynchronous calls
 *
 * @param ops number of operations. zero: as many as possible
 *
 * Check if any URPC request has finished.
 * If yes, pop a cmd from inflight queue, receive its result.
 * Push a new command, if any.
 * Repeat.
 */
void ThreadContext::progress(int ops)
{
  std::lock_guard<std::mutex> lock(this->prog_mtx);
  _progress_nolock(ops);
}

/**
 * @brief Synchronize this context.
 *
 * Block other threads from submitting requests to this context,
 * call progress() until request queue and inflight queues are empty.
 */
void ThreadContext::synchronize()
{
  std::lock_guard<std::mutex> lock(this->submit_mtx);
  this->_synchronize_nolock();
}
  
/**
 * @brief The actual synchronize work function
 *
 * This function should only be called with the main_mutex locked!
 */
void ThreadContext::_synchronize_nolock()
{
  while(!(this->comq.emptyRequest() && this->comq.emptyInFlight())) {
    this->progress(0);
  }
}

/**
 * @brief call a VE function asynchronously
 *
 * @param addr VEMVA of VE function to call
 * @param args arguments of the function
 * @return request ID
 */
uint64_t ThreadContext::callAsync(uint64_t addr, CallArgs &args)
{
  if ( addr == 0 || this->state == VEO_STATE_EXIT)
    return VEO_REQUEST_ID_INVALID;
  
  auto id = this->issueRequestID();
  //
  // submit function, called when cmd is issued to URPC
  //
  auto f = [&args, this, addr, id] (Command *cmd)
           {
             VEO_TRACE(this, "[request #%d] start...", id);
             int req = send_call_nolock(this->up, this->ve_sp, addr, args);
             VEO_TRACE(this, "[request #%d] VE-URPC req ID = %ld", id, req);
             if (req >= 0) {
               cmd->setURPCReq(req, VEO_COMMAND_UNFINISHED);
             } else {
               // TODO: anything more meaningful into result?
               cmd->setResult(0, VEO_COMMAND_ERROR);
               return -EAGAIN;
             }
             return 0;
           };

  //
  // result function, called when response has arrived from URPC
  //
  auto u = [&args, this, id] (Command *cmd, urpc_mb_t *m, void *payload, size_t plen)
           {
             VEO_TRACE(this, "[request #%d] reply sendbuff received (cmd=%d)...", id, m->c.cmd);
             uint64_t result;
             int rv = unpack_call_result(m, &args, payload, plen, &result);
             VEO_TRACE(this, "[request #%d] unpacked", id);
             if (rv < 0) {
               cmd->setResult(result, VEO_COMMAND_EXCEPTION);
               return rv;
             }
             cmd->setResult(result, VEO_COMMAND_OK);
             return 0;
           };

  std::unique_ptr<Command> cmd(new internal::CommandImpl(id, f, u));
  {
    std::lock_guard<std::mutex> lock(this->submit_mtx);
    if(this->comq.pushRequest(std::move(cmd)))
      return VEO_REQUEST_ID_INVALID;
  }
  this->progress(3);
  return id;
}

/**
 * @brief call a VE function specified by symbol name asynchronously
 *
 * @param libhdl handle of library
 * @param symname a symbol name to find
 * @param args arguments of the function
 * @return request ID
 */
uint64_t ThreadContext::callAsyncByName(uint64_t libhdl, const char *symname, CallArgs &args)
{
  uint64_t addr = this->proc->getSym(libhdl, symname);
  return this->callAsync(addr, args);
}

/**
 * @brief call a VH function asynchronously
 *
 * @param func address of VH function to call
 * @param arg pointer to opaque arguments structure for the function
 * @return request ID
 */
uint64_t ThreadContext::callVHAsync(uint64_t (*func)(void *), void *arg)
{
  if ( func == nullptr || this->state == VEO_STATE_EXIT)
    return VEO_REQUEST_ID_INVALID;

  auto id = this->issueRequestID();
  auto f = [this, func, arg, id] (Command *cmd)
           {
             VEO_TRACE(this, "[request #%lu] start...", id);
             auto rv = (*func)(arg);
             VEO_TRACE(this, "[request #%lu] executed. (return %ld)", id, rv);
             cmd->setResult(rv, VEO_COMMAND_OK);
             VEO_TRACE(this, "[request #%lu] done", id);
             return 0;
           };
  std::unique_ptr<Command> req(new internal::CommandImpl(id, f));
  {
    std::lock_guard<std::mutex> lock(this->submit_mtx);
    this->comq.pushRequest(std::move(req));
  }
  this->progress(3);
  return id;
}

/**
 * @brief check if the result of a request (command) is available
 *
 * @param reqid request ID to wait
 * @param retp pointer to buffer to store the return value.
 * @retval VEO_COMMAND_OK the execution of the function succeeded.
 * @retval VEO_COMMAND_EXCEPTION exception occured on the execution.
 * @retval VEO_COMMAND_ERROR error occured on handling the command.
 * @retval VEO_COMMAND_UNFINISHED the command is not finished.
 */
int ThreadContext::callPeekResult(uint64_t reqid, uint64_t *retp)
{
  this->progress(3);
  std::lock_guard<std::mutex> lock(this->req_mtx);
  auto itr = rem_reqid.find(reqid);
  if( itr == rem_reqid.end() ) {
    return VEO_COMMAND_ERROR;
  }
  auto c = this->comq.peekCompletion(reqid);
  if (c != nullptr) {
    if (!rem_reqid.erase(reqid))
      return VEO_COMMAND_ERROR;
    *retp = c->getRetval();
    return c->getStatus();
  }
  return VEO_COMMAND_UNFINISHED;
}

/**
 * @brief wait for the result of request (command)
 *
 * @param reqid request ID to wait
 * @param retp pointer to buffer to store the return value.
 * @retval VEO_COMMAND_OK the execution of the function succeeded.
 * @retval VEO_COMMAND_EXCEPTION exception occured on the execution.
 * @retval VEO_COMMAND_ERROR error occured on handling the command.
 * @retval VEO_COMMAND_UNFINISHED the command is not finished.
 */
int ThreadContext::callWaitResult(uint64_t reqid, uint64_t *retp)
{
#if 1
  //
  // polling here because we need to call the progress function!
  //
  int rv;
  do {
    rv = this->callPeekResult(reqid, retp);
  } while (rv == VEO_COMMAND_UNFINISHED);
  return rv;
#else
  req_mtx.lock();
  auto itr = rem_reqid.find(reqid);
  if( itr == rem_reqid.end() ) {
    req_mtx.unlock();
    return VEO_COMMAND_ERROR;
  }
  if (!rem_reqid.erase(reqid)) {
    req_mtx.unlock();
    return VEO_COMMAND_ERROR;
  }
  req_mtx.unlock();
  auto c = this->comq.waitCompletion(reqid);
  *retp = c->getRetval();
  return c->getStatus();
#endif
}

} // namespace veo