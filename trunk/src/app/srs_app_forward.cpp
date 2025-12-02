//
// Copyright (c) 2013-2025 The SRS Authors
//
// SPDX-License-Identifier: MIT
//

#include <srs_app_forward.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <stdlib.h>
#include <sys/socket.h>

using namespace std;

#include <srs_app_config.hpp>
#include <srs_app_factory.hpp>
#include <srs_app_rtmp_conn.hpp>
#include <srs_app_rtmp_source.hpp>
#include <srs_app_st.hpp>
#include <srs_app_utility.hpp>
#include <srs_core_autofree.hpp>
#include <srs_kernel_codec.hpp>
#include <srs_kernel_error.hpp>
#include <srs_kernel_kbps.hpp>
#include <srs_kernel_log.hpp>
#include <srs_kernel_pithy_print.hpp>
#include <srs_kernel_utility.hpp>
#include <srs_protocol_amf0.hpp>
#include <srs_protocol_rtmp_msg_array.hpp>
#include <srs_protocol_rtmp_stack.hpp>
#include <srs_protocol_utility.hpp>

ISrsForwarder::ISrsForwarder()
{
}

ISrsForwarder::~ISrsForwarder()
{
}

SrsForwarder::SrsForwarder(ISrsOriginHub *h)
{
    hub_ = h;

    req_ = NULL;
    sh_video_ = sh_audio_ = NULL;

    sdk_ = NULL;
    trd_ = new SrsDummyCoroutine();
    queue_ = new SrsMessageQueue();
    jitter_ = new SrsRtmpJitter();

    app_factory_ = _srs_app_factory;
    config_ = _srs_config;
}

SrsForwarder::~SrsForwarder()
{
    srs_freep(sdk_);
    srs_freep(trd_);
    srs_freep(queue_);
    srs_freep(jitter_);

    srs_freep(sh_video_);
    srs_freep(sh_audio_);

    srs_freep(req_);

    app_factory_ = NULL;
    config_ = NULL;
}

srs_error_t SrsForwarder::initialize(ISrsRequest *r, string ep)
{
    srs_error_t err = srs_success;

    // it's ok to use the request object,
    // SrsLiveSource already copy it and never delete it.
    req_ = r->copy();

    // the ep(endpoint) to forward to
    ep_forward_ = ep;

    // Check if the forward destination is RTMPS URL
    // SRS forward only supports plain RTMP protocol, not RTMPS (RTMP over SSL/TLS)
    if (ep_forward_.find("rtmps://") != string::npos) {
        return srs_error_new(ERROR_NOT_SUPPORTED, "forward does not support RTMPS destination=%s", ep_forward_.c_str());
    }

    // Remember the source context id.
    source_cid_ = _srs_context->get_id();

    return err;
}

void SrsForwarder::set_queue_size(srs_utime_t queue_size)
{
    queue_->set_queue_size(queue_size);
}

srs_error_t SrsForwarder::on_publish()
{
    srs_error_t err = srs_success;

    srs_freep(trd_);
    trd_ = new SrsSTCoroutine("forward", this);
    if ((err = trd_->start()) != srs_success) {
        return srs_error_wrap(err, "start thread");
    }

    return err;
}

void SrsForwarder::on_unpublish()
{
    trd_->stop();
    if (sdk_)
        sdk_->close();
}

srs_error_t SrsForwarder::on_meta_data(SrsMediaPacket *shared_metadata)
{
    srs_error_t err = srs_success;

    SrsMediaPacket *metadata = shared_metadata->copy();

    // Use ZERO jitter algorithm to ensure timestamps start from 0 for external services
    if ((err = jitter_->correct(metadata, SrsRtmpJitterAlgorithmZERO)) != srs_success) {
        return srs_error_wrap(err, "jitter");
    }

    if ((err = queue_->enqueue(metadata)) != srs_success) {
        return srs_error_wrap(err, "enqueue metadata");
    }

    return err;
}

srs_error_t SrsForwarder::on_audio(SrsMediaPacket *shared_audio)
{
    srs_error_t err = srs_success;

    SrsMediaPacket *msg = shared_audio->copy();

    // Use ZERO jitter algorithm to ensure timestamps start from 0 for external services
    if ((err = jitter_->correct(msg, SrsRtmpJitterAlgorithmZERO)) != srs_success) {
        return srs_error_wrap(err, "jitter");
    }

    if (SrsFlvAudio::sh(msg->payload(), msg->size())) {
        srs_freep(sh_audio_);
        sh_audio_ = msg->copy();
    }

    if ((err = queue_->enqueue(msg)) != srs_success) {
        return srs_error_wrap(err, "enqueue audio");
    }

    return err;
}

srs_error_t SrsForwarder::on_video(SrsMediaPacket *shared_video)
{
    srs_error_t err = srs_success;

    SrsMediaPacket *msg = shared_video->copy();

    // Use ZERO jitter algorithm to ensure timestamps start from 0 for external services
    if ((err = jitter_->correct(msg, SrsRtmpJitterAlgorithmZERO)) != srs_success) {
        return srs_error_wrap(err, "jitter");
    }

    if (SrsFlvVideo::sh(msg->payload(), msg->size())) {
        srs_freep(sh_video_);
        sh_video_ = msg->copy();
    }

    if ((err = queue_->enqueue(msg)) != srs_success) {
        return srs_error_wrap(err, "enqueue video");
    }

    return err;
}

// when error, forwarder sleep for a while and retry.
#define SRS_FORWARDER_CIMS (3 * SRS_UTIME_SECONDS)

srs_error_t SrsForwarder::cycle()
{
    srs_error_t err = srs_success;

    srs_trace("Forwarder: Start forward %s of source=[%s] to %s",
              req_->get_stream_url().c_str(), source_cid_.c_str(), ep_forward_.c_str());

    while (true) {
        // We always check status first.
        // @see https://github.com/ossrs/srs/issues/1634#issuecomment-597571561
        if ((err = trd_->pull()) != srs_success) {
            return srs_error_wrap(err, "forwarder");
        }

        if ((err = do_cycle()) != srs_success) {
            srs_warn("Forwarder: Ignore error, %s", srs_error_desc(err).c_str());
            srs_freep(err);
        }

        // Never wait if thread error, fast quit.
        // @see https://github.com/ossrs/srs/pull/2284
        if ((err = trd_->pull()) != srs_success) {
            return srs_error_wrap(err, "forwarder");
        }

        srs_usleep(SRS_FORWARDER_CIMS);
    }

    return err;
}

srs_error_t SrsForwarder::do_cycle()
{
    srs_error_t err = srs_success;

    std::string url;
    if (true) {
        std::string server;
        int port = SRS_CONSTS_RTMP_DEFAULT_PORT;

        // parse host:port from hostport.
        srs_net_split_hostport(ep_forward_, server, port);

        // Generate clean RTMP URL for forwarding.
        // For external services like Amazon IVS, YouTube, Twitch, etc.,
        // we should NOT append vhost parameter as they don't understand it.
        // Format: rtmp://server:port/app/stream
        std::stringstream ss;
        ss << "rtmp://" << server << ":" << port << "/" << req_->app_ << "/" << req_->stream_;
        
        // Only append original params if they exist and don't contain vhost
        // (vhost is SRS-specific and external services don't understand it)
        if (!req_->param_.empty()) {
            std::string param = req_->param_;
            // Remove vhost from params for external forwarding
            size_t vhost_pos = param.find("vhost=");
            if (vhost_pos == std::string::npos) {
                // No vhost in param, safe to append
                if (param[0] != '?' && param[0] != '&') {
                    ss << "?";
                }
                ss << param;
            }
        }
        url = ss.str();
    }

    srs_trace("Forwarder: Connecting to %s, app=%s, stream=%s", url.c_str(), req_->app_.c_str(), req_->stream_.c_str());

    srs_freep(sdk_);
    srs_utime_t cto = SRS_FORWARDER_CIMS;
    srs_utime_t sto = SRS_CONSTS_RTMP_TIMEOUT;
    sdk_ = app_factory_->create_rtmp_client(url, cto, sto);

    if ((err = sdk_->connect()) != srs_success) {
        return srs_error_wrap(err, "sdk connect url=%s, cto=%dms, sto=%dms.", url.c_str(), srsu2msi(cto), srsu2msi(sto));
    }

    // For external services like Amazon IVS, we need to publish with the exact stream name
    // without any vhost parameter. Use a conservative chunk size (4096) for compatibility.
    string stream;
    int chunk_size = 4096;  // Use standard chunk size for external services
    if ((err = sdk_->publish(chunk_size, false, &stream)) != srs_success) {
        return srs_error_wrap(err, "sdk publish");
    }
    
    srs_trace("Forwarder: Published successfully, actual_stream=%s, chunk_size=%d", stream.c_str(), chunk_size);

    if ((err = hub_->on_forwarder_start(this)) != srs_success) {
        return srs_error_wrap(err, "notify hub start");
    }

    if ((err = forward()) != srs_success) {
        return srs_error_wrap(err, "forward");
    }

    srs_trace("forward publish url %s, stream=%s%s as %s", url.c_str(), req_->stream_.c_str(), req_->param_.c_str(), stream.c_str());

    return err;
}

#define SYS_MAX_FORWARD_SEND_MSGS 128
srs_error_t SrsForwarder::forward()
{
    srs_error_t err = srs_success;

    sdk_->set_recv_timeout(SRS_CONSTS_RTMP_PULSE);

    SrsUniquePtr<SrsPithyPrint> pprint(SrsPithyPrint::create_forwarder());

    SrsMessageArray msgs(SYS_MAX_FORWARD_SEND_MSGS);

    // update sequence header
    // Reset timestamp to 0 for sequence headers to ensure compatibility with external services
    srs_trace("Forwarder: Sending sequence headers, video_sh=%s(%d bytes, ts=%d), audio_sh=%s(%d bytes, ts=%d)",
              sh_video_ ? "yes" : "no", sh_video_ ? sh_video_->size() : 0, sh_video_ ? (int)sh_video_->timestamp_ : 0,
              sh_audio_ ? "yes" : "no", sh_audio_ ? sh_audio_->size() : 0, sh_audio_ ? (int)sh_audio_->timestamp_ : 0);
    
    if (sh_video_) {
        SrsMediaPacket* video_copy = sh_video_->copy();
        video_copy->timestamp_ = 0;  // Reset timestamp for external services
        if ((err = sdk_->send_and_free_message(video_copy)) != srs_success) {
            return srs_error_wrap(err, "send video sh");
        }
        srs_trace("Forwarder: Video sequence header sent successfully");
    }
    if (sh_audio_) {
        SrsMediaPacket* audio_copy = sh_audio_->copy();
        audio_copy->timestamp_ = 0;  // Reset timestamp for external services
        if ((err = sdk_->send_and_free_message(audio_copy)) != srs_success) {
            return srs_error_wrap(err, "send audio sh");
        }
        srs_trace("Forwarder: Audio sequence header sent successfully");
    }

    int total_msgs_sent = 0;
    
    while (true) {
        if ((err = trd_->pull()) != srs_success) {
            return srs_error_wrap(err, "thread quit");
        }

        pprint->elapse();

        // read from client.
        if (true) {
            SrsRtmpCommonMessage *msg = NULL;
            err = sdk_->recv_message(&msg);

            if (err != srs_success && srs_error_code(err) != ERROR_SOCKET_TIMEOUT) {
                srs_warn("Forwarder: Connection lost after sending %d messages", total_msgs_sent);
                return srs_error_wrap(err, "receive control message");
            }
            srs_freep(err);

            srs_freep(msg);
        }

        // forward all messages.
        // each msg in msgs.msgs_ must be free, for the SrsMessageArray never free them.
        int count = 0;
        if ((err = queue_->dump_packets(msgs.max_, msgs.msgs_, count)) != srs_success) {
            return srs_error_wrap(err, "dump packets");
        }

        // pithy print
        if (pprint->can_print()) {
            sdk_->kbps_sample(SRS_CONSTS_LOG_FOWARDER, pprint->age(), count);
        }

        // ignore when no messages.
        if (count <= 0) {
            continue;
        }

        total_msgs_sent += count;
        
        // sendout messages, all messages are freed by send_and_free_messages().
        if ((err = sdk_->send_and_free_messages(msgs.msgs_, count)) != srs_success) {
            return srs_error_wrap(err, "send messages");
        }
    }

    return err;
}
