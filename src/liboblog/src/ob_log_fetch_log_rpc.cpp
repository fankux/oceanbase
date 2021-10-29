/**
 * Copyright (c) 2021 OceanBase
 * OceanBase CE is licensed under Mulan PubL v2.
 * You can use this software according to the terms and conditions of the Mulan PubL v2.
 * You may obtain a copy of Mulan PubL v2 at:
 *          http://license.coscl.org.cn/MulanPubL-2.0
 * THIS SOFTWARE IS PROVIDED ON AN "AS IS" BASIS, WITHOUT WARRANTIES OF ANY KIND,
 * EITHER EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO NON-INFRINGEMENT,
 * MERCHANTABILITY OR FIT FOR A PARTICULAR PURPOSE.
 * See the Mulan PubL v2 for more details.
 */

#define USING_LOG_PREFIX OBLOG_FETCHER

#include "ob_log_fetch_log_rpc.h"

#include <unistd.h>                       // getpid
#include "share/ob_errno.h"                 // OB_SUCCESS
#include "lib/atomic/ob_atomic.h"         // ATOMIC_*
#include "lib/utility/ob_macro_utils.h"   // OB_FAIL
#include "lib/oblog/ob_log_module.h"      // LOG_ERROR
#include "lib/allocator/ob_malloc.h"      // ob_malloc/ob_free

#include "ob_log_rpc.h"                   // IObLogRpc
#include "ob_log_stream_worker.h"         // IObLogStreamWorker
#include "ob_log_fetch_stream.h"          // FetchStream
#include "ob_log_trace_id.h"              // ObLogTraceIdGuard
#include "ob_log_config.h"                // ObLogConfig

using namespace oceanbase::common;
using namespace oceanbase::obrpc;

namespace oceanbase
{
namespace liboblog
{

///////////////////////////// OpenStreamSRpc ////////////////////////

OpenStreamSRpc::OpenStreamSRpc() :
    req_(),
    resp_(),
    rcode_()
{}

OpenStreamSRpc::~OpenStreamSRpc()
{
  reset();
}

void OpenStreamSRpc::reset()
{
  req_.reset();
  resp_.reset();
  rcode_.reset();
}

int OpenStreamSRpc::open_stream(IObLogRpc &rpc,
      const common::ObAddr &svr,
      const int64_t timeout,
      FetchTaskList &task_list,
      const obrpc::ObStreamSeq &stale_stream_seq,
      const int64_t stream_life_time)
{
  int ret = OB_SUCCESS;

  // build request
  if (OB_FAIL(build_request_(task_list, stale_stream_seq, stream_life_time))) {
    LOG_ERROR("build request fail", KR(ret), K(task_list), K(stale_stream_seq), K(stream_life_time));
  } else {
    ret = launch_open_stream_rpc_(rpc, svr, timeout);
  }
  return ret;
}

int OpenStreamSRpc::open_stream(IObLogRpc &rpc,
    const common::ObAddr &svr,
    const int64_t timeout,
    const common::ObPartitionKey &pkey,
    const uint64_t next_log_id,
    const obrpc::ObStreamSeq &stale_stream_seq,
    const int64_t stream_life_time)
{
  int ret = OB_SUCCESS;
  // build request
  if (OB_FAIL(build_request_(pkey, next_log_id, stale_stream_seq, stream_life_time))) {
    LOG_ERROR("build request fail", KR(ret), K(pkey), K(next_log_id), K(stale_stream_seq),
        K(stream_life_time));
  } else {
    ret = launch_open_stream_rpc_(rpc, svr, timeout);
  }
  return ret;
}

int OpenStreamSRpc::launch_open_stream_rpc_(IObLogRpc &rpc,
    const common::ObAddr &svr,
    const int64_t timeout)
{
  int ret = OB_SUCCESS;

  // send open stream rpc
  if(OB_FAIL(rpc.open_stream(svr, req_, resp_, timeout))) {
    LOG_ERROR("send open stream rpc fail", KR(ret), K(svr), K(req_), K(resp_), K(timeout));

    // set error code
    rcode_.rcode_ = ret;
    (void)snprintf(rcode_.msg_, sizeof(rcode_.msg_), "send open stream rpc fail");

    // If the RPC send fails, the server is considered invalid directly and the fetch log server should be switched
    // Reset the error code
    ret = OB_SUCCESS;
  } else {
    // RPC success
  }

  return ret;
}

int OpenStreamSRpc::build_request_(FetchTaskList &part_list,
    const obrpc::ObStreamSeq &stale_stream_seq,
    const int64_t stream_life_time)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(set_request_basic_param_(stale_stream_seq, stream_life_time))) {
    LOG_ERROR("set request basic param fail", KR(ret), K(stale_stream_seq), K(stream_life_time));
  } else if (OB_FAIL(set_request_part_list_(part_list))) {
    LOG_ERROR("set request part list fail", KR(ret), K(part_list));
  }
  return ret;
}

int OpenStreamSRpc::build_request_(const common::ObPartitionKey &pkey,
    const uint64_t next_log_id,
    const obrpc::ObStreamSeq &stale_stream_seq,
    const int64_t stream_life_time)
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(set_request_basic_param_(stale_stream_seq, stream_life_time))) {
    LOG_ERROR("set request basic param fail", KR(ret), K(stale_stream_seq), K(stream_life_time));
  } else if (OB_FAIL(set_request_part_list_(pkey, next_log_id))) {
    LOG_ERROR("set request part list fail", KR(ret), K(pkey), K(next_log_id));
  }
  return ret;
}

int OpenStreamSRpc::set_request_basic_param_(const obrpc::ObStreamSeq &stale_stream_seq,
    const int64_t stream_life_time)
{
  int ret = OB_SUCCESS;

  //////////////// set request parameters /////////////////
  // Set the outdated stream identifier to allow the server side to remove the outdated stream structure
  req_.set_stale_stream(stale_stream_seq);

  // set stream lifetime
  req_.set_stream_lifetime(stream_life_time);

  // set pid of liboblog progress
  req_.set_liboblog_pid(static_cast<uint64_t>(getpid()));
  return ret;
}

int OpenStreamSRpc::set_request_part_list_(FetchTaskList &part_list)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(part_list.count() <= 0) || OB_ISNULL(part_list.head())) {
    LOG_ERROR("part list is empty", K(part_list));
    ret = OB_INVALID_ARGUMENT;
  } else {
    PartFetchCtx *task = part_list.head();

    // Iterate through the task list and set the fetch_log_request list
    while (OB_SUCCESS == ret && NULL != task) {
      obrpc::ObLogOpenStreamReq::Param param;
      param.pkey_ = task->get_pkey();
      param.start_log_id_ = task->get_next_log_id();

      if (OB_FAIL(req_.append_param(param))) {
        LOG_ERROR("open stream request append param fail", KR(ret), K(param), K(req_));
      } else {
        task = task->get_next();
      }
    }
  }
  return ret;
}

int OpenStreamSRpc::set_request_part_list_(const common::ObPartitionKey &pkey,
      const uint64_t next_log_id)
{
  int ret = OB_SUCCESS;
  obrpc::ObLogOpenStreamReq::Param param;
  param.pkey_ = pkey;
  param.start_log_id_ = next_log_id;

  if (OB_FAIL(req_.append_param(param))) {
    LOG_ERROR("open stream request append param fail", KR(ret), K(param), K(req_));
  }
  return ret;
}

////////////////////////////// FetchLogSRpc //////////////////////////////
FetchLogSRpc::FetchLogSRpc() :
    req_(),
    resp_(),
    rcode_(),
    cb_(*this),
    cond_(),
    rpc_done_(false)
{
}

FetchLogSRpc::~FetchLogSRpc()
{
  reset();
}

void FetchLogSRpc::reset()
{
  req_.reset();
  resp_.reset();
  rcode_.reset();
  rpc_done_ = false;
}

int FetchLogSRpc::build_request_(const obrpc::ObStreamSeq &seq,
    const int64_t upper_limit,
    const int64_t fetch_log_cnt_per_part_per_round,
    const bool need_feed_back)
{
  int ret = OB_SUCCESS;
  reset();

  // Set request parameters
  req_.set_upper_limit_ts(upper_limit);
  req_.set_log_cnt_per_part_per_round(fetch_log_cnt_per_part_per_round);
  req_.set_stream_seq(seq);
  req_.set_feedback(need_feed_back);
  return ret;
}

int FetchLogSRpc::fetch_log(IObLogRpc &rpc,
    const common::ObAddr &svr,
    const int64_t timeout,
    const obrpc::ObStreamSeq &seq,
    const int64_t upper_limit,
    const int64_t fetch_log_cnt_per_part_per_round,
    const bool need_feed_back)
{
  int ret = OB_SUCCESS;

  reset();

  // build request
  if (OB_FAIL(build_request_(seq, upper_limit, fetch_log_cnt_per_part_per_round, need_feed_back))) {
    LOG_ERROR("build request fail", KR(ret), K(seq), K(upper_limit),
        K(fetch_log_cnt_per_part_per_round), K(need_feed_back));
  }
  // Send asynchronous fetch log RPC
  else if (OB_FAIL(rpc.async_stream_fetch_log(svr, req_, cb_, timeout))) {
    LOG_ERROR("send async stream fetch log rpc fail", KR(ret), K(svr), K(req_), K(timeout));

    rcode_.rcode_ = ret;
    (void)snprintf(rcode_.msg_, sizeof(rcode_.msg_), "send async stream fetch log rpc fail");

    // RPC send fails, finish directly
    rpc_done_ = true;

    // RPC sending errors are always considered to be Server problems and require switching servers
    // Reset the error code
    ret = OB_SUCCESS;
  } else {
    // If the RPC is sent successfully, block waits for rpc done
    while (! ATOMIC_LOAD(&rpc_done_)) {
      // This relies on the fact that the RPC must eventually call back, so the TIMEOUT time is not set
      cond_.wait();
    }
  }

  return ret;
}

int FetchLogSRpc::set_resp(const obrpc::ObRpcResultCode &rcode,
    const obrpc::ObLogStreamFetchLogResp *resp)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(OB_SUCCESS == rcode.rcode_ && NULL == resp)) {
    LOG_ERROR("invalid fetch log response", K(rcode), K(resp));
    ret = OB_INVALID_ARGUMENT;
  } else {
    rcode_ = rcode;

    if (OB_SUCCESS == rcode.rcode_) {
      if (OB_FAIL(resp_.assign(*resp))) {
        LOG_ERROR("assign new fetch log resp fail", KR(ret), KPC(resp), K(resp_));
      }
    }
  }

  if (OB_SUCCESS == ret) {
    // mark rpc done
    ATOMIC_SET(&rpc_done_, true);
    cond_.signal();
  }
  return ret;
}

////////////////////////////// FetchLogSRpc::RpcCB //////////////////////////////
FetchLogSRpc::RpcCB::RpcCB(FetchLogSRpc &host) : host_(host)
{}

FetchLogSRpc::RpcCB::~RpcCB()
{}

rpc::frame::ObReqTransport::AsyncCB *FetchLogSRpc::RpcCB::clone(const rpc::frame::SPAlloc &alloc) const
{
  void *buf = NULL;
  RpcCB *cb = NULL;

  if (OB_ISNULL(buf = alloc(sizeof(RpcCB)))) {
    LOG_ERROR("clone rpc callback fail", K(buf), K(sizeof(RpcCB)));
  } else if (OB_ISNULL(cb = new(buf) RpcCB(host_))) {
    LOG_ERROR("construct RpcCB fail", K(buf));
  } else {
    // success
  }

  return cb;
}

int FetchLogSRpc::RpcCB::process()
{
  int ret = OB_SUCCESS;
  ObLogStreamFetchLogResp &result = RpcCBBase::result_;
  ObRpcResultCode &rcode = RpcCBBase::rcode_;
  const common::ObAddr &svr = RpcCBBase::dst_;

  if (OB_FAIL(do_process_(rcode, &result))) {
    LOG_ERROR("process fetch log callback fail", KR(ret), K(result), K(rcode), K(svr));
  }
  // Note: destruct response after asynchronous RPC processing
  result.reset();

  return ret;
}

void FetchLogSRpc::RpcCB::on_timeout()
{
  int ret = OB_SUCCESS;
  ObRpcResultCode rcode;
  const common::ObAddr &svr = RpcCBBase::dst_;

  rcode.rcode_ = OB_TIMEOUT;
  (void)snprintf(rcode.msg_, sizeof(rcode.msg_), "fetch log rpc timeout, svr=%s",
      to_cstring(svr));

  if (OB_FAIL(do_process_(rcode, NULL))) {
    LOG_ERROR("process fetch log callback on timeout fail", KR(ret), K(rcode), K(svr));
  }
}

void FetchLogSRpc::RpcCB::on_invalid()
{
  int ret = OB_SUCCESS;
  ObRpcResultCode rcode;
  const common::ObAddr &svr = RpcCBBase::dst_;

  // 遇到无效的包，decode失败
  rcode.rcode_ = OB_RPC_PACKET_INVALID;
  (void)snprintf(rcode.msg_, sizeof(rcode.msg_),
      "fetch log rpc response packet is invalid, svr=%s",
      to_cstring(svr));

  if (OB_FAIL(do_process_(rcode, NULL))) {
    LOG_ERROR("process fetch log callback on invalid fail", KR(ret), K(rcode), K(svr));
  }
}

int FetchLogSRpc::RpcCB::do_process_(const ObRpcResultCode &rcode, const ObLogStreamFetchLogResp *resp)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(OB_SUCCESS == rcode.rcode_ && NULL == resp)) {
    LOG_ERROR("invalid response packet", K(rcode), K(resp));
    ret = OB_INVALID_ERROR;
  }
  else if (OB_FAIL(host_.set_resp(rcode, resp))) {
    LOG_ERROR("set fetch log response fail", KR(ret), K(resp), K(rcode));
  } else {
    // success
  }
  return ret;
}

////////////////////////////// FetchLogARpc //////////////////////////////

int64_t FetchLogARpc::g_rpc_result_count_per_rpc_upper_limit =
    ObLogConfig::default_rpc_result_count_per_rpc_upper_limit;

bool FetchLogARpc::g_print_rpc_handle_info = ObLogConfig::default_print_rpc_handle_info;

void FetchLogARpc::configure(const ObLogConfig &config)
{
  int64_t rpc_result_count_per_rpc_upper_limit = config.rpc_result_count_per_rpc_upper_limit;
  bool print_rpc_handle_info = config.print_rpc_handle_info;

  ATOMIC_STORE(&g_rpc_result_count_per_rpc_upper_limit, rpc_result_count_per_rpc_upper_limit);
  LOG_INFO("[CONFIG]", K(rpc_result_count_per_rpc_upper_limit));
  ATOMIC_STORE(&g_print_rpc_handle_info, print_rpc_handle_info);
  LOG_INFO("[CONFIG]", K(print_rpc_handle_info));
}

const char *FetchLogARpc::print_rpc_stop_reason(const RpcStopReason reason)
{
  const char *reason_str = "INVALID";
  switch (reason) {
    case INVALID_REASON:
      reason_str = "INVALID";
      break;

    case REACH_MAX_LOG:
      reason_str = "REACH_MAX_LOG";
      break;

    case REACH_UPPER_LIMIT:
      reason_str = "REACH_UPPER_LIMIT";
      break;

    case FETCH_NO_LOG:
      reason_str = "FETCH_NO_LOG";
      break;

    case FETCH_LOG_FAIL:
      reason_str = "FETCH_LOG_FAIL";
      break;

    case REACH_MAX_RPC_RESULT:
      reason_str = "REACH_MAX_RPC_RESULT";
      break;

    case FORCE_STOP_RPC:
      reason_str = "FORCE_STOP_RPC";
      break;

    default:
      reason_str = "INVALID";
      break;
  }

  return reason_str;
}

FetchLogARpc::FetchLogARpc(FetchStream &host) :
    host_(host),
    svr_(),
    rpc_(NULL),
    stream_worker_(NULL),
    result_pool_(NULL),
    state_(IDLE),
    cur_req_(NULL),
    flying_req_list_(),
    res_queue_(),
    lock_()
{
  int ret = OB_SUCCESS;
  if (OB_FAIL(res_queue_.init(ObModIds::OB_LOG_FETCH_LOG_ARPC_RES_QUEUE))) {
    LOG_ERROR("init result queue fail", KR(ret));
  }
}

FetchLogARpc::~FetchLogARpc()
{
  reset();
  res_queue_.destroy();
}

void FetchLogARpc::reset()
{
  // Wait for all asynchronous RPCs to complete
  stop();

  svr_.reset();
  rpc_ = NULL;
  stream_worker_ = NULL;
  result_pool_ = NULL;
  state_ = IDLE;
  cur_req_ = NULL;
  flying_req_list_.reset();
  (void)res_queue_.reset();
}

void FetchLogARpc::reset(const common::ObAddr &svr,
    IObLogRpc &rpc,
    IObLogStreamWorker &stream_worker,
    IFetchLogARpcResultPool &result_pool)
{
  reset();

  svr_ = svr;
  rpc_ = &rpc;
  stream_worker_ = &stream_worker;
  result_pool_ = &result_pool;
}

int FetchLogARpc::prepare_request(const obrpc::ObStreamSeq &seq,
      const int64_t part_cnt,
      const int64_t fetch_log_cnt_per_part_per_round,
      const bool need_feed_back,
      const int64_t rpc_timeout)
{
  int ret = OB_SUCCESS;
  ObSpinLockGuard guard(lock_);

  void *fetch_stream = &host_;
  LOG_INFO("[STAT] [FETCH_LOG_ARPC] prepare rpc request", K(fetch_stream),
      "stream_seq", seq,
      "ready_result", res_queue_.count(),
      "flying_rpc", flying_req_list_.count_,
      K(part_cnt), K(fetch_log_cnt_per_part_per_round), K(need_feed_back), K(rpc_timeout));

  // Requires IDLE status
  if (OB_UNLIKELY(IDLE != state_)) {
    LOG_ERROR("state not match which is not IDLE", K(state_));
    ret = OB_STATE_NOT_MATCH;
  }
  // Requirement to discard previous requests first
  else if (OB_UNLIKELY(NULL != cur_req_)) {
    LOG_ERROR("request has not been discarded", K(cur_req_), KPC(cur_req_));
    ret = OB_INVALID_ERROR;
  }
  // Allocate a new RPC request to carry the new stream
  else if (OB_FAIL(alloc_rpc_request_(seq, part_cnt,
      fetch_log_cnt_per_part_per_round,
      need_feed_back,
      rpc_timeout,
      cur_req_))) {
    LOG_ERROR("alloc rpc request fail", KR(ret), K(seq), K(part_cnt),
        K(fetch_log_cnt_per_part_per_round), K(need_feed_back));
  } else if (OB_ISNULL(cur_req_)) {
    LOG_ERROR("alloc rpc request fail", K(cur_req_));
    ret = OB_ERR_UNEXPECTED;
  } else {
    // success
  }
  return ret;
}

// Requires repeatable calls
void FetchLogARpc::discard_request(const char *discard_reason)
{
  ObSpinLockGuard guard(lock_);

  // Reset current request
  if (NULL != cur_req_) {
    void *fetch_stream = &host_;
    LOG_INFO("[STAT] [FETCH_LOG_ARPC] discard rpc request", K(discard_reason),
        K(fetch_stream), K_(cur_req), KPC_(cur_req),
        "ready_result", res_queue_.count(),
        "flying_rpc", flying_req_list_.count_);

    // If the RPC has finished, the request structure is cleaned up directly
    if (! cur_req_->rpc_is_flying()) {
      free_rpc_request_(cur_req_);
    } else {
      // Add to the run list if the RPC has not finished
      flying_req_list_.add(cur_req_);
    }

    cur_req_ = NULL;

    // Clear all requests and then set the status to IDLE
    clear_result_();
  }
}

int FetchLogARpc::async_fetch_log(const obrpc::ObStreamSeq &seq,
    const int64_t upper_limit,
    bool &rpc_send_succeed)
{
  int ret = OB_SUCCESS;
  ObSpinLockGuard guard(lock_);

  rpc_send_succeed = false;

  // Requires IDLE status
  if (OB_UNLIKELY(IDLE != state_)) {
    LOG_ERROR("state not match which is not IDLE", K(state_));
    ret = OB_STATE_NOT_MATCH;
  }
  // Requires the request structure to be prepared
  else if (OB_ISNULL(cur_req_)) {
    LOG_ERROR("current request is invalid", K(cur_req_));
    ret = OB_INVALID_ERROR;
  }
  // Request matching stream
  else if (OB_UNLIKELY(seq != cur_req_->get_stream_seq())) {
    LOG_ERROR("stream not match", "rpc_stream_seq", cur_req_->get_stream_seq(),
        "request_stream_seq", seq, KPC(cur_req_));
    ret = OB_INVALID_ERROR;
  }
  // Requirement that no RPC requests are being executed
  else if (OB_UNLIKELY(cur_req_->rpc_is_flying())) {
    LOG_ERROR("RPC is flying, can not launch async fetch log request",
        K(cur_req_->rpc_is_flying()), KPC(cur_req_));
    ret = OB_INVALID_ERROR;
  }
    // Initiating asynchronous requests
  else if (OB_FAIL(launch_async_rpc_(*cur_req_, upper_limit, false, rpc_send_succeed))) {
    LOG_ERROR("launch async rpc fail", KR(ret), K(upper_limit), KPC(cur_req_));
  } else {
    // success
  }

  return ret;
}

int64_t FetchLogARpc::get_flying_request_count()
{
  ObSpinLockGuard guard(lock_);
  return flying_req_list_.count_;
}

void FetchLogARpc::print_flying_request_list()
{
  ObSpinLockGuard guard(lock_);
  RpcRequest *req = flying_req_list_.head_;
  int64_t index = 0;
  while (NULL != req) {
    LOG_INFO("[FLYING_RPC_REQUEST]", "fetch_stream", &host_, K_(svr), K(index), K(req), KPC(req));
    req = req->next_;
    index++;
  }
}

void FetchLogARpc::stop()
{
  int64_t wait_count = 0;

  // Note: this function does not lock itself, it locks in the following sub-functions

  // Dispose the current request
  discard_request("stop");

  // Wait for the request list to be empty, i.e. all asynchronous RPCs have been completed
  if (get_flying_request_count() > 0) {
    LOG_INFO("wait for flying async fetch log rpc done",
        "fetch_stream", &host_, K_(svr),
        "flying_request_count", get_flying_request_count());
    print_flying_request_list();

    int64_t start_time = get_timestamp();
    while (get_flying_request_count() > 0) {
      wait_count++;
      if (0 == (wait_count % WARN_COUNT_ON_STOP)) {
        LOG_WARN("wait for flying async fetch log rpc done",
            "fetch_stream", &host_, K_(svr),
            "flying_request_count", get_flying_request_count(),
            "wait_time", get_timestamp() - start_time);
        print_flying_request_list();
      }

      usec_sleep(WAIT_TIME_ON_STOP);
    }

    LOG_INFO("all flying async fetch log rpc is done",
        "fetch_stream", &host_, K_(svr),
        "wait_time", get_timestamp() - start_time,
        "flying_request_count", get_flying_request_count());
  }
}

int FetchLogARpc::next_result(FetchLogARpcResult *&result, bool &rpc_is_flying)
{
  int ret = OB_SUCCESS;
  ObSpinLockGuard guard(lock_);

  // Requires current RPC request to be valid
  if (OB_ISNULL(cur_req_)) {
    LOG_ERROR("current request is invalid", K(cur_req_));
    ret = OB_INVALID_ERROR;
  }
  // Requires status is READY
  else if (OB_UNLIKELY(READY != state_)) {
    LOG_ERROR("state not match which is not READY", K(state_));
    ret = OB_STATE_NOT_MATCH;
  } else if (OB_ISNULL(result_pool_)) {
    LOG_ERROR("invalid rpc result pool", K(result_pool_));
    ret = OB_INVALID_ERROR;
  } else {
    rpc_is_flying = cur_req_->rpc_is_flying();

    result = NULL;
    while (OB_SUCCESS == ret && NULL == result) {
      if (OB_FAIL(pop_result_(result))) {
        if (OB_EAGAIN == ret) {
          // Iteration complete
          ret = OB_ITER_END;
        } else {
          LOG_ERROR("pop result fail", KR(ret));
        }
      } else if (OB_ISNULL(result)) {
        LOG_ERROR("invalid result", K(result));
        ret = OB_ERR_UNEXPECTED;
      } else if (result->seq_ != cur_req_->get_stream_seq()) {
        // Filtering data that is not the current stream
        result_pool_->free(result);
        result = NULL;
      } else {
        // success
      }
    }
  }

  return ret;
}

void FetchLogARpc::revert_result(FetchLogARpcResult *result)
{
  ObSpinLockGuard guard(lock_);

  if (OB_ISNULL(result_pool_)) {
    LOG_ERROR("invalid rpc result pool", K(result_pool_));
  } else if (OB_NOT_NULL(result)) {
    result_pool_->free(result);
    result = NULL;
  }
}

int FetchLogARpc::update_request(const obrpc::ObStreamSeq &seq,
    const int64_t upper_limit,
    const int64_t fetch_log_cnt_per_part_per_round,
    const bool need_feed_back,
    const int64_t rpc_timeout)
{
  int ret = OB_SUCCESS;
  ObSpinLockGuard guard(lock_);

  // Requires current RPC request to be valid
  if (OB_ISNULL(cur_req_)) {
    LOG_ERROR("current request is invalid", K(cur_req_));
    ret = OB_INVALID_ERROR;
  } else if (OB_UNLIKELY(seq != cur_req_->get_stream_seq())) {
    LOG_ERROR("stream does not match", K(seq), K(cur_req_->get_stream_seq()));
    ret = OB_INVALID_ARGUMENT;
  } else {
    // Update request parameters
    cur_req_->update_request(upper_limit,
        fetch_log_cnt_per_part_per_round,
        need_feed_back,
        rpc_timeout);
  }

  return ret;
}

int FetchLogARpc::mark_request_stop(const obrpc::ObStreamSeq &seq)
{
  int ret = OB_SUCCESS;
  ObSpinLockGuard guard(lock_);

  // Requires current RPC request to be valid
  if (OB_ISNULL(cur_req_)) {
    LOG_ERROR("current request is invalid", K(cur_req_));
    ret = OB_INVALID_ERROR;
  } else if (OB_UNLIKELY(seq != cur_req_->get_stream_seq())) {
    LOG_ERROR("stream does not match", K(seq), K(cur_req_->get_stream_seq()));
    ret = OB_INVALID_ARGUMENT;
  } else if (cur_req_->rpc_is_flying()) {
    // If the RPC is running, mark the next round of RPCs to stop
    cur_req_->mark_stop_flag();
  } else {
    // RPC stop case, no need to mark stop
  }
  return ret;
}

int FetchLogARpc::handle_rpc_response(RpcRequest &rpc_req,
    const obrpc::ObRpcResultCode &rcode,
    const obrpc::ObLogStreamFetchLogResp *resp)
{
  int ret = OB_SUCCESS;
  int64_t start_proc_time = get_timestamp();

  // Locked mutually exclusive access
  ObSpinLockGuard lock_guard(lock_);

  // Use the trace id associated with the request
  ObLogTraceIdGuard guard(rpc_req.get_trace_id());

  if (OB_ISNULL(stream_worker_)) {
    LOG_ERROR("invalid stream worker", K(stream_worker_));
    ret = OB_INVALID_ERROR;
  }
  // Verify that the request is the same as the current request: that is, whether it is the same object
  else if (NULL == cur_req_ || cur_req_ != &rpc_req) {
    // RPC requests have been deprecated
    LOG_INFO("rpc request has been discarded", K_(svr),
        "fetch_stream", &host_, K(rpc_req), KPC(cur_req_));

    // Try to find the corresponding RPC request structure, then destroy the
    if (OB_FAIL(destroy_flying_request_(&rpc_req))) {
      LOG_ERROR("destroy_flying_request_ fail", KR(ret), K(rpc_req));
    }
  } else {
    bool need_dispatch_stream_task = false;
    bool need_stop_rpc = false;
    RpcStopReason rpc_stop_reason = INVALID_REASON;
    int64_t next_upper_limit = OB_INVALID_TIMESTAMP;

    // Analyze the results and make decisions accordingly
    // 1. Decide if the next RPC needs to be launched
    // 2. Decide the upper limit of the next RPC
    //
    // TODO: Make more decisions based on request results
    //  1. Decide whether to dynamically modify the upper limit interval based on whether the upper limit is reached or not
    if (OB_FAIL(analyze_result_(rpc_req, rcode, resp, need_stop_rpc, rpc_stop_reason,
        next_upper_limit))) {
      LOG_ERROR("analyze_result_ fail", KR(ret), K(rpc_req), K(rcode), K(resp));
    }
    // Generate RPC results and determine if a log fetching task needs to be assigned
    else if (OB_FAIL(generate_rpc_result_(rpc_req, rcode, resp, start_proc_time,
        need_stop_rpc,
        rpc_stop_reason,
        need_dispatch_stream_task))) {
      LOG_ERROR("generate_rpc_result_ fail", KR(ret), K(rpc_req), K(rcode), K(resp),
          K(start_proc_time), K(need_stop_rpc), K(rpc_stop_reason));
    } else {
      // Print monitoring logs
      print_handle_info_(rpc_req, resp, next_upper_limit, need_stop_rpc, rpc_stop_reason,
          need_dispatch_stream_task);

      if (need_stop_rpc) {
        // If you do not need to continue fetching logs, mark the RPC as not running
        rpc_req.mark_flying_state(false);
      } else {
        bool rpc_send_succeed = false;
        // Launch the next RPC
        if (OB_FAIL(launch_async_rpc_(rpc_req, next_upper_limit, true, rpc_send_succeed))) {
          LOG_ERROR("launch_async_rpc_ fail", KR(ret), K(rpc_req), K(next_upper_limit));
        }
      }

      // Assign log stream fetching tasks as needed
      if (OB_SUCCESS == ret && need_dispatch_stream_task) {
        if (OB_FAIL(stream_worker_->dispatch_stream_task(host_, "RpcCallback"))) {
          LOG_ERROR("dispatch stream task fail", KR(ret));
        }
      }
    }
  }

  return ret;
}

const char *FetchLogARpc::print_state(State state)
{
  const char *str = "UNKNOWN";
  switch (state) {
    case IDLE:
      str = "IDLE";
      break;
    case READY:
      str = "READY";
      break;
    default:
      str = "UNKNOWN";
      break;
  }
  return str;
}

int FetchLogARpc::alloc_rpc_request_(const obrpc::ObStreamSeq &seq,
    const int64_t part_cnt,
    const int64_t fetch_log_cnt_per_part_per_round,
    const bool need_feed_back,
    const int64_t rpc_timeout,
    RpcRequest *&req)
{
  int ret = OB_SUCCESS;
  int64_t size = sizeof(RpcRequest);
  void *buf = NULL;

  if (OB_UNLIKELY(part_cnt <= 0)) {
    LOG_ERROR("invalid rpc partition count", K(part_cnt));
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_ISNULL(buf = ob_malloc(size, ObModIds::OB_LOG_FETCH_LOG_ARPC_REQUEST))) {
    LOG_ERROR("allocate memory for RpcRequest fail", K(size));
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else if (OB_ISNULL(req = new(buf) RpcRequest(*this, seq, part_cnt,
      fetch_log_cnt_per_part_per_round,
      need_feed_back,
      rpc_timeout))) {
    LOG_ERROR("construct RpcRequest fail", K(buf), K(req), K(part_cnt),
        K(fetch_log_cnt_per_part_per_round), K(need_feed_back));
    ret = OB_ALLOCATE_MEMORY_FAILED;
  } else {
    // success
  }

  return ret;
}

void FetchLogARpc::free_rpc_request_(RpcRequest *request)
{
  if (OB_NOT_NULL(request)) {
    request->~RpcRequest();
    ob_free(request);
    request = NULL;
  }
}

int FetchLogARpc::generate_rpc_result_(RpcRequest &rpc_req,
    const obrpc::ObRpcResultCode &rcode,
    const obrpc::ObLogStreamFetchLogResp *resp,
    const int64_t rpc_callback_start_time,
    const bool need_stop_rpc,
    const RpcStopReason rpc_stop_reason,
    bool &need_dispatch_stream_task)
{
  int ret = OB_SUCCESS;
  FetchLogARpcResult *result = NULL;
  bool is_state_idle = false;
  int64_t rpc_start_time = rpc_req.get_rpc_start_time();
  const obrpc::ObStreamSeq &seq = rpc_req.get_stream_seq();
  const common::ObCurTraceId::TraceId &trace_id = rpc_req.get_trace_id();

  if (OB_ISNULL(result_pool_)) {
    LOG_ERROR("invalid result pool", K(result_pool_));
    ret = OB_INVALID_ERROR;
  }
  // Assign an RPC result
  else if (OB_FAIL(result_pool_->alloc(result))) {
    LOG_ERROR("alloc rpc result fail", KR(ret));
  } else if (OB_ISNULL(result)) {
    LOG_ERROR("invalid result", K(result));
    ret = OB_ERR_UNEXPECTED;
  } else if (OB_FAIL(result->set(seq, rcode, resp, trace_id,
      rpc_start_time,
      rpc_callback_start_time,
      need_stop_rpc,
      rpc_stop_reason))) {
    LOG_ERROR("set rpc result fail", KR(ret), K(seq), K(rcode), KPC(resp), K(rpc_start_time),
        K(rpc_callback_start_time), K(need_stop_rpc), K(rpc_stop_reason));
    result_pool_->free(result);
    result = NULL;
  }
  // Push the results to the queue and modify the status at the same time
  else if (OB_FAIL(push_result_and_be_ready_(result, is_state_idle))) {
    LOG_ERROR("push result and be ready fail", KR(ret), KPC(result));
  } else {
    // If the status is IDLE before inserting the result, the task needs to be assigned
    need_dispatch_stream_task = is_state_idle;
  }

  // Reclaiming memory in case of failure
  if (OB_SUCCESS != ret && NULL != result_pool_ && NULL != result) {
    result_pool_->free(result);
    result = NULL;
  }

  return ret;
}

void FetchLogARpc::print_handle_info_(RpcRequest &rpc_req,
    const obrpc::ObLogStreamFetchLogResp *resp,
    const int64_t next_upper_limit,
    const bool need_stop_rpc,
    const RpcStopReason rpc_stop_reason,
    const bool need_dispatch_stream_task)
{
  const bool print_rpc_handle_info = ATOMIC_LOAD(&g_print_rpc_handle_info);
  int64_t part_count = rpc_req.get_part_count();
  int64_t req_upper_limit = rpc_req.get_upper_limit();
  int64_t rpc_time = get_timestamp() - rpc_req.get_rpc_start_time();
  const ObStreamSeq &stream_seq = rpc_req.get_stream_seq();
  void *fetch_stream = &host_;

  if (print_rpc_handle_info) {
    LOG_INFO("handle rpc result by rpc callback",
        K(fetch_stream), K(stream_seq), K(part_count),
        K(need_stop_rpc),
        "stop_reason", print_rpc_stop_reason(rpc_stop_reason),
        "ready_result", res_queue_.count(),
        "max_result", ATOMIC_LOAD(&g_rpc_result_count_per_rpc_upper_limit),
        K(need_dispatch_stream_task),
        "flying_rpc", flying_req_list_.count_,
        "upper_limit", TS_TO_STR(req_upper_limit),
        "delta", next_upper_limit - req_upper_limit,
        K(rpc_time), KPC(resp));
  } else {
    LOG_DEBUG("handle rpc result by rpc callback",
        K(fetch_stream), K(stream_seq), K(part_count),
        K(need_stop_rpc),
        "stop_reason", print_rpc_stop_reason(rpc_stop_reason),
        "ready_result", res_queue_.count(),
        "max_result", ATOMIC_LOAD(&g_rpc_result_count_per_rpc_upper_limit),
        K(need_dispatch_stream_task),
        "flying_rpc", flying_req_list_.count_,
        "upper_limit", TS_TO_STR(req_upper_limit),
        "delta", next_upper_limit - req_upper_limit,
        K(rpc_time), KPC(resp));
  }
}

int FetchLogARpc::launch_async_rpc_(RpcRequest &rpc_req,
    const int64_t upper_limit,
    const bool launch_by_cb,
    bool &rpc_send_succeed)
{
  int ret = OB_SUCCESS;

  rpc_send_succeed = false;

  if (OB_ISNULL(rpc_)) {
    LOG_ERROR("invalid handlers", K(rpc_));
    ret = OB_INVALID_ERROR;
  } else {
    // First prepare the RPC request and update the upper limit
    rpc_req.prepare(upper_limit);

    // Use the trace id of the request
    ObLogTraceIdGuard guard(rpc_req.get_trace_id());

    _LOG_DEBUG("launch async fetch log rpc by %s, request=%s",
        launch_by_cb ? "callback" : "fetch stream", to_cstring(rpc_req));

    // The default setting is flyin before sending an asynchronous request
    // The reason for not setting it up after sending is that there is a concurrency problem after successful sending,
    // and the RPC request may have come back before it is set up
    rpc_req.mark_flying_state(true);

    // Sending the asynchronous RPC
    ret = rpc_->async_stream_fetch_log(svr_, rpc_req.req_, rpc_req.cb_, rpc_req.rpc_timeout_);

    if (OB_SUCC(ret)) {
      // RPC sent successfully
      // You can't continue to manipulate the related structures afterwards
      rpc_send_succeed = true;
    } else {
      int64_t start_proc_time = get_timestamp();

      // First reset the ret return value
      int err_code = ret;
      ret = OB_SUCCESS;

      // RPC send failure
      rpc_send_succeed = false;

      // Mark RPC is not running
      rpc_req.mark_flying_state(false);

      // Set error code
      ObRpcResultCode rcode;
      rcode.rcode_ = err_code;
      (void)snprintf(rcode.msg_, sizeof(rcode.msg_), "send async stream fetch log rpc fail");

      LOG_ERROR("send async stream fetch log rpc fail", KR(ret), K(svr_), K(rpc_req), K(launch_by_cb));

      // RPC send failure, uniformly considered to be a problem of the observer, directly generated RPC results
      bool rpc_stopped = true;
      RpcStopReason reason = FETCH_LOG_FAIL;
      // Note: No need to process the return value here: need_dispatch_stream_task
      // This function initiates the RPC request, and only determines whether the dispatch task is needed
      // when the RPC result is processed by the asynchronous callback
      bool need_dispatch_stream_task = false;
      if (OB_FAIL(generate_rpc_result_(rpc_req, rcode, NULL, start_proc_time,
          rpc_stopped, reason, need_dispatch_stream_task))) {
        LOG_ERROR("generate rpc result fail", KR(ret), K(rpc_req), K(rcode), K(start_proc_time),
            K(rpc_stopped), K(reason));
      }
    }
  }
  return ret;
}

int FetchLogARpc::push_result_and_be_ready_(FetchLogARpcResult *result, bool &is_state_idle)
{
  int ret = OB_SUCCESS;
  if (OB_ISNULL(result)) {
    LOG_ERROR("invalid argument", K(result));
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(res_queue_.push(result))) {
    LOG_ERROR("push result into queue fail", KR(ret), K(result));
  } else {
    is_state_idle = (IDLE == state_);

    // After generating the data, modify the status and set it to READY unconditionally
    if (READY != state_) {
      (void)ATOMIC_SET(&state_, READY);
    }
  }
  return ret;
}

int FetchLogARpc::pop_result_(FetchLogARpcResult *&result)
{
  int ret = OB_SUCCESS;
  void *data = NULL;

  ret = res_queue_.pop(data);
  result = static_cast<FetchLogARpcResult *>(data);

  if (OB_SUCCESS == ret) {
    // success
  } else if (OB_EAGAIN == ret) {
    // No data
    // Change the status to IDLE
    (void)ATOMIC_SET(&state_, IDLE);
  } else {
    LOG_ERROR("pop result from queue fail", KR(ret));
  }
  return ret;
}

void FetchLogARpc::clear_result_()
{
  int ret = OB_SUCCESS;
  void *data = NULL;

  // Empty the data when the queue is not empty
  if (res_queue_.count() > 0) {
    // Require result_pool_ be valid
    if (OB_ISNULL(result_pool_)) {
      LOG_ERROR("invalid rpc result pool, can not clear results", K(result_pool_));
    } else {
      while (OB_SUCC(res_queue_.pop(data))) {
        FetchLogARpcResult *result = static_cast<FetchLogARpcResult *>(data);
        if (OB_NOT_NULL(result)) {
          result_pool_->free(result);
          result = NULL;
        }
        data = NULL;
      }
    }
  }

  // Status forced to IDLE
  (void)ATOMIC_SET(&state_, IDLE);
}

int FetchLogARpc::destroy_flying_request_(RpcRequest *target_request)
{
  int ret = OB_SUCCESS;

  // Remove the request from the run list
  // If the request is not in the list, an exception must have occurred
  if (OB_ISNULL(target_request)) {
    LOG_ERROR("invalid argument", K(target_request));
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_FAIL(flying_req_list_.remove(target_request))) {
    LOG_ERROR("remove rpc request from flying request list fail", KR(ret), K(target_request));
  } else {
    free_rpc_request_(target_request);
    target_request = NULL;
  }

  return ret;
}

int FetchLogARpc::analyze_result_(RpcRequest &rpc_req,
    const obrpc::ObRpcResultCode &rcode,
    const obrpc::ObLogStreamFetchLogResp *resp,
    bool &need_stop_rpc,
    RpcStopReason &rpc_stop_reason,
    int64_t &next_upper_limit)
{
  int ret = OB_SUCCESS;
  const int64_t rpc_result_count_per_rpc_upper_limit =
      ATOMIC_LOAD(&g_rpc_result_count_per_rpc_upper_limit);
  const int64_t cur_rpc_result_count = res_queue_.count();
  bool reach_max_rpc_result = (cur_rpc_result_count >= rpc_result_count_per_rpc_upper_limit);
  bool force_stop_rpc = rpc_req.get_stop_flag();

  need_stop_rpc = false;
  rpc_stop_reason = INVALID_REASON;
  next_upper_limit = rpc_req.get_upper_limit();

  // If the RPC send fails, or the server returns a failure, there is no need to continue fetching logs
  if (OB_SUCCESS != rcode.rcode_ || NULL == resp || OB_SUCCESS != resp->get_err()) {
    need_stop_rpc = true;
    rpc_stop_reason = FETCH_LOG_FAIL;
  } else if (reach_max_rpc_result) {
    // If the number of RPC results reaches the threshold, stop sending RPCs
    need_stop_rpc = true;
    rpc_stop_reason = REACH_MAX_RPC_RESULT;
  } else if (force_stop_rpc) {
    // External forced stop RPC
    need_stop_rpc = true;
    rpc_stop_reason = FORCE_STOP_RPC;
  } else {
    const ObFetchStatus &fetch_status = resp->get_fetch_status();

    int64_t last_upper_limit = rpc_req.get_upper_limit();
    int64_t total_part_count = rpc_req.get_part_count();
    int64_t reach_upper_limit_count = fetch_status.reach_upper_limit_ts_pkey_count_;
    int64_t reach_max_log_id_count = fetch_status.reach_max_log_id_pkey_count_;

    // All partitions reach maximum log
    bool all_reach_max_log = (reach_max_log_id_count >= total_part_count);

    // All partitions reach one of two states: either the maximum log is reached, or the progress limit is reached
    bool reach_max_log_or_upper_limit =
        ((reach_upper_limit_count + reach_max_log_id_count) >= total_part_count);

    bool fetch_no_log = (resp->get_log_num() <= 0);

    // If all partitions have reached the maximum log, there is no need to continue fetching logs
    if (all_reach_max_log) {
      need_stop_rpc = true;
      rpc_stop_reason = REACH_MAX_LOG;
    } else if (reach_max_log_or_upper_limit) {
      // For partitions that have reached their upper limit, get the latest upper limit
      // Decide whether to continue to send RPC by whether the upper limit has changed
      if (OB_FAIL(host_.get_upper_limit(next_upper_limit))) {
        LOG_ERROR("get upper limit fail", KR(ret));
      } else {
        need_stop_rpc = (next_upper_limit <= last_upper_limit);
        rpc_stop_reason = REACH_UPPER_LIMIT;
      }
    } else if (fetch_no_log) {
      // No logs need to be fetched next time, if no logs are fetched
      need_stop_rpc = true;
      rpc_stop_reason = FETCH_NO_LOG;
    } else {
      // The logs are requested to continue to be fetched in all other cases
      need_stop_rpc = false;
      rpc_stop_reason = INVALID_REASON;
    }
  }
  return ret;
}

///////////////////////////// FetchLogARpc::RpcCB /////////////////////////

rpc::frame::ObReqTransport::AsyncCB *FetchLogARpc::RpcCB::clone(const rpc::frame::SPAlloc &alloc) const
{
  void *buf = NULL;
  RpcCB *cb = NULL;

  if (OB_ISNULL(buf = alloc(sizeof(RpcCB)))) {
    LOG_ERROR("clone rpc callback fail", K(buf), K(sizeof(RpcCB)));
  } else if (OB_ISNULL(cb = new(buf) RpcCB(host_))) {
    LOG_ERROR("construct RpcCB fail", K(buf));
  } else {
    // 成功
  }

  return cb;
}

int FetchLogARpc::RpcCB::process()
{
  int ret = OB_SUCCESS;
  ObLogStreamFetchLogResp &result = RpcCBBase::result_;
  ObRpcResultCode &rcode = RpcCBBase::rcode_;
  const common::ObAddr &svr = RpcCBBase::dst_;

  if (OB_FAIL(do_process_(rcode, &result))) {
    LOG_ERROR("process fetch log callback fail", KR(ret), K(result), K(rcode), K(svr), K_(host));
  }
  // Note: Active destructe response after asynchronous RPC processing
  result.reset();

  return ret;
}

void FetchLogARpc::RpcCB::on_timeout()
{
  int ret = OB_SUCCESS;
  ObRpcResultCode rcode;
  const common::ObAddr &svr = RpcCBBase::dst_;

  rcode.rcode_ = OB_TIMEOUT;
  (void)snprintf(rcode.msg_, sizeof(rcode.msg_), "fetch log rpc timeout, svr=%s",
      to_cstring(svr));

  if (OB_FAIL(do_process_(rcode, NULL))) {
    LOG_ERROR("process fetch log callback on timeout fail", KR(ret), K(rcode), K(svr), K_(host));
  }
}

void FetchLogARpc::RpcCB::on_invalid()
{
  int ret = OB_SUCCESS;
  ObRpcResultCode rcode;
  const common::ObAddr &svr = RpcCBBase::dst_;

  // Invalid package encountered, decode failed
  rcode.rcode_ = OB_RPC_PACKET_INVALID;
  (void)snprintf(rcode.msg_, sizeof(rcode.msg_),
      "fetch log rpc response packet is invalid, svr=%s",
      to_cstring(svr));

  if (OB_FAIL(do_process_(rcode, NULL))) {
    LOG_ERROR("process fetch log callback on invalid fail", KR(ret), K(rcode), K(svr), K_(host));
  }
}

int FetchLogARpc::RpcCB::do_process_(const ObRpcResultCode &rcode, const ObLogStreamFetchLogResp *resp)
{
  int ret = OB_SUCCESS;
  RpcRequest &rpc_req = host_;
  FetchLogARpc &rpc_host = rpc_req.host_;

  if (OB_UNLIKELY(OB_SUCCESS == rcode.rcode_ && NULL == resp)) {
    LOG_ERROR("invalid response packet", K(rcode), K(resp));
    ret = OB_INVALID_ERROR;
  }
  // Processing RPC response results
  else if (OB_FAIL(rpc_host.handle_rpc_response(rpc_req, rcode, resp))) {
    LOG_ERROR("set fetch log response fail", KR(ret), K(resp), K(rcode), K(rpc_req));
  } else {
    // success
  }
  return ret;
}

///////////////////////////// FetchLogARpc::RpcRequest /////////////////////////

FetchLogARpc::RpcRequest::RpcRequest(FetchLogARpc &host,
    const obrpc::ObStreamSeq &seq,
    const int64_t part_cnt,
    const int64_t fetch_log_cnt_per_part_per_round,
    const bool need_feed_back,
    const int64_t rpc_timeout) :
    host_(host),
    part_cnt_(part_cnt),
    cb_(*this),
    need_feed_back_(false),
    rpc_timeout_(rpc_timeout),
    req_(),
    trace_id_(),
    next_(NULL),
    force_stop_flag_(false),
    rpc_start_time_(OB_INVALID_TIMESTAMP),
    rpc_is_flying_(false)
{
  // 初始化请求
  req_.set_stream_seq(seq);
  req_.set_log_cnt_per_part_per_round(fetch_log_cnt_per_part_per_round);
  req_.set_feedback(need_feed_back);
}

void FetchLogARpc::RpcRequest::update_request(const int64_t upper_limit,
    const int64_t fetch_log_cnt_per_part_per_round,
    const bool need_feed_back,
    const int64_t rpc_timeout)
{
  req_.set_upper_limit_ts(upper_limit);
  req_.set_log_cnt_per_part_per_round(fetch_log_cnt_per_part_per_round);
  need_feed_back_ = need_feed_back;
  rpc_timeout_ = rpc_timeout;
}

void FetchLogARpc::RpcRequest::prepare(const int64_t upper_limit)
{
  // set request parameter: upper limit
  // upper limit may need reset before rpc send, thus value of upper limit should be provided dynamicly
  //
  // Set request parameter: upper limit
  req_.set_upper_limit_ts(upper_limit);

  // Update the next round of RPC trace id
  trace_id_.init(get_self_addr());

  // Update request time
  rpc_start_time_ = get_timestamp();

  // reset stop flag
  force_stop_flag_ = false;

  // Set whether feedback is required
  req_.set_feedback(need_feed_back_);
  if (need_feed_back_) {
    // Set the feedback only once
    need_feed_back_ = false;
  }
}

void FetchLogARpc::RpcRequest::mark_flying_state(const bool is_flying)
{
  ATOMIC_SET(&rpc_is_flying_, is_flying);
}

////////////////////////////// RpcRequestList //////////////////////////////

void FetchLogARpc::RpcRequestList::add(RpcRequest *req)
{
  if (OB_NOT_NULL(req)) {
    req->next_ = NULL;

    if (NULL == head_) {
      head_ = req;
      tail_ = req;
    } else {
      tail_->next_ = req;
      tail_ = req;
    }

    count_++;
  }
}

int FetchLogARpc::RpcRequestList::remove(RpcRequest *target)
{
  int ret = OB_SUCCESS;

  if (OB_ISNULL(target)) {
    ret = OB_INVALID_ARGUMENT;
  } else {
    bool found = false;
    RpcRequest *pre_request = NULL;
    RpcRequest *request = head_;

    // Find the matching request structure
    while (NULL != request && ! found) {
      if (target == request) {
        found = true;
      } else {
        pre_request = request;
        request = request->next_;
      }
    }

    if (found) {
      // Delete the corresponding node
      if (NULL == pre_request) {
        head_ = target->next_;
        if (target == tail_) {
          tail_ = head_;
        }
      } else {
        pre_request->next_ = target->next_;
        if (target == tail_) {
          tail_ = pre_request;
        }
      }

      count_--;
    } else {
      ret = OB_ENTRY_NOT_EXIST;
    }
  }

  return ret;
}


////////////////////////////// FetchLogARpcResult //////////////////////////////

int FetchLogARpcResult::set(const obrpc::ObStreamSeq &seq,
    const obrpc::ObRpcResultCode &rcode,
    const obrpc::ObLogStreamFetchLogResp *resp,
    const common::ObCurTraceId::TraceId &trace_id,
    const int64_t rpc_start_time,
    const int64_t rpc_callback_start_time,
    const bool rpc_stop_upon_result,
    const FetchLogARpc::RpcStopReason rpc_stop_reason)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(OB_SUCCESS == rcode.rcode_ && NULL == resp)) {
    LOG_ERROR("invalid fetch log response", K(rcode), K(resp));
    ret = OB_INVALID_ARGUMENT;
  } else if (OB_UNLIKELY(rpc_start_time <= 0) || OB_UNLIKELY(rpc_callback_start_time <= 0)) {
    LOG_ERROR("invalid argument", K(rpc_start_time), K(rpc_callback_start_time));
    ret = OB_INVALID_ARGUMENT;
  } else {
    seq_ = seq;
    rcode_ = rcode;
    trace_id_ = trace_id;

    // The result is valid when no error occurs
    if (OB_SUCCESS == rcode.rcode_) {
      if (OB_FAIL(resp_.assign(*resp))) {
        LOG_ERROR("assign new fetch log resp fail", KR(ret), KPC(resp), K(resp_));
      }
    } else {
      resp_.reset();
    }
  }

  // After setting all the result items, only then start setting the statistics items,
  // because the results need a memory copy and this time must be considered
  if (OB_SUCCESS == ret) {
    int64_t rpc_end_time = get_timestamp();

    rpc_time_ = rpc_end_time - rpc_start_time;
    rpc_callback_time_ = rpc_end_time - rpc_callback_start_time;
    rpc_stop_upon_result_ = rpc_stop_upon_result;
    rpc_stop_reason_ = rpc_stop_reason;
  }
  return ret;
}

////////////////////////////// FetchLogARpcResult Object Pool //////////////////////////////

int FetchLogARpcResultPool::init(const int64_t cached_obj_count)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(inited_)) {
    LOG_ERROR("init twice");
    ret = OB_INIT_TWICE;
  } else if (OB_FAIL(pool_.init(cached_obj_count,
      ObModIds::OB_LOG_FETCH_LOG_ARPC_RESULT,
      OB_SERVER_TENANT_ID,
      DEFAULT_RESULT_POOL_BLOCK_SIZE))) {
    LOG_ERROR("init result obj pool fail", KR(ret), K(cached_obj_count));
  } else {
    inited_ = true;
  }
  return ret;
}

void FetchLogARpcResultPool::destroy()
{
  inited_ = false;
  pool_.destroy();
}

void FetchLogARpcResultPool::print_stat()
{
  int64_t alloc_count = pool_.get_alloc_count();
  int64_t free_count = pool_.get_free_count();
  int64_t fixed_count = pool_.get_fixed_count();
  int64_t used_count = alloc_count - free_count;
  int64_t dynamic_count = (alloc_count > fixed_count) ? alloc_count - fixed_count : 0;

  _LOG_INFO("[STAT] [RPC_RESULT_POOL] USED=%ld FREE=%ld FIXED=%ld DYNAMIC=%ld",
      used_count, free_count, fixed_count, dynamic_count);
}

int FetchLogARpcResultPool::alloc(FetchLogARpcResult *&result)
{
  int ret = OB_SUCCESS;
  if (OB_UNLIKELY(! inited_)) {
    ret = OB_NOT_INIT;
  } else {
    ret = pool_.alloc(result);
  }
  return ret;
}

void FetchLogARpcResultPool::free(FetchLogARpcResult *result)
{
  int ret = OB_SUCCESS;
  if (OB_LIKELY(inited_) && OB_NOT_NULL(result)) {
    result->reset();
    if (OB_FAIL(pool_.free(result))) {
      LOG_ERROR("free result into pool fail", KR(ret), K(result));
    } else {
      result = NULL;
    }
  }
}


}
}
