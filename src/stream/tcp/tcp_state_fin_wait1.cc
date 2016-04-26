//--------------------------------------------------------------------------
// Copyright (C) 2015-2016 Cisco and/or its affiliates. All rights reserved.
//
// This program is free software; you can redistribute it and/or modify it
// under the terms of the GNU General Public License Version 2 as published
// by the Free Software Foundation.  You may not use, modify or distribute
// this program under any other version of the GNU General Public License.
//
// This program is distributed in the hope that it will be useful, but
// WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
// General Public License for more details.
//
// You should have received a copy of the GNU General Public License along
// with this program; if not, write to the Free Software Foundation, Inc.,
// 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
//--------------------------------------------------------------------------

// tcp_state_fin_wait1.cc author davis mcpherson <davmcphe@cisco.com>
// Created on: Aug 5, 2015

#include <iostream>
using namespace std;

#include "tcp_module.h"
#include "tcp_tracker.h"
#include "tcp_session.h"
#include "tcp_normalizer.h"
#include "tcp_state_fin_wait1.h"

#ifdef UNIT_TEST
#include "catch/catch.hpp"
#endif

TcpStateFinWait1::TcpStateFinWait1(TcpStateMachine& tsm) :
    TcpStateHandler(TcpStreamTracker::TCP_FIN_WAIT1, tsm)
{
}

TcpStateFinWait1::~TcpStateFinWait1()
{
}

bool TcpStateFinWait1::syn_sent(TcpSegmentDescriptor& tsd, TcpStreamTracker& trk)
{
    trk.session->check_for_repeated_syn(tsd);

    return default_state_action(tsd, trk);
}

bool TcpStateFinWait1::syn_recv(TcpSegmentDescriptor& tsd, TcpStreamTracker& trk)
{
    trk.normalizer->ecn_tracker(tsd.get_tcph(), trk.session->config->require_3whs() );
    if ( tsd.get_seg_len() )
        trk.session->handle_data_on_syn(tsd);

    return default_state_action(tsd, trk);
}

bool TcpStateFinWait1::syn_ack_sent(TcpSegmentDescriptor& tsd, TcpStreamTracker& trk)
{
    return default_state_action(tsd, trk);
}

bool TcpStateFinWait1::syn_ack_recv(TcpSegmentDescriptor& tsd, TcpStreamTracker& trk)
{
    if ( tsd.get_seg_len() )
        trk.session->handle_data_on_syn(tsd);

    return default_state_action(tsd, trk);
}

bool TcpStateFinWait1::ack_sent(TcpSegmentDescriptor& tsd, TcpStreamTracker& trk)
{
    trk.update_tracker_ack_sent(tsd);

    return default_state_action(tsd, trk);
}

bool TcpStateFinWait1::ack_recv(TcpSegmentDescriptor& tsd, TcpStreamTracker& trk)
{
    trk.update_tracker_ack_recv(tsd);
    check_for_window_slam(tsd, trk);

    return default_state_action(tsd, trk);
}

bool TcpStateFinWait1::data_seg_sent(TcpSegmentDescriptor& tsd, TcpStreamTracker& trk)
{
    trk.update_tracker_ack_sent(tsd);

    return default_state_action(tsd, trk);
}

bool TcpStateFinWait1::data_seg_recv(TcpSegmentDescriptor& tsd, TcpStreamTracker& trk)
{
    trk.update_tracker_ack_recv(tsd);
    if ( check_for_window_slam(tsd, trk) )
    {
        if ( tsd.get_seg_len() > 0 )
            trk.session->handle_data_segment(tsd);
    }

    return default_state_action(tsd, trk);
}

bool TcpStateFinWait1::fin_sent(TcpSegmentDescriptor& tsd, TcpStreamTracker& trk)
{
    trk.update_tracker_ack_sent(tsd);

    return default_state_action(tsd, trk);
}

bool TcpStateFinWait1::fin_recv(TcpSegmentDescriptor& tsd, TcpStreamTracker& trk)
{
    Flow* flow = tsd.get_flow();

    trk.update_tracker_ack_recv(tsd);
    trk.update_on_fin_recv(tsd);

    if ( check_for_window_slam(tsd, trk) )
    {
        //session.handle_fin_recv_in_fw1(tsd);
        if ( tsd.get_seg_len() > 0 )
            trk.session->handle_data_segment(tsd);

        if ( !flow->two_way_traffic() )
            trk.set_tf_flags(TF_FORCE_FLUSH);

        trk.set_tcp_state(TcpStreamTracker::TCP_TIME_WAIT);
    }

    return default_state_action(tsd, trk);
}

bool TcpStateFinWait1::rst_sent(TcpSegmentDescriptor& tsd, TcpStreamTracker& trk)
{
    return default_state_action(tsd, trk);
}

bool TcpStateFinWait1::rst_recv(TcpSegmentDescriptor& tsd, TcpStreamTracker& trk)
{
    if ( trk.update_on_rst_recv(tsd) )
    {
        trk.session->update_session_on_rst(tsd, true);
        trk.session->update_perf_base_state(TcpStreamTracker::TCP_CLOSING);
        trk.session->set_pkt_action_flag(ACTION_RST);
    }
    else
    {
        trk.session->tel.set_tcp_event(EVENT_BAD_RST);
    }

    // FIXIT - might be good to create alert specific to RST with data
    if ( tsd.get_seg_len() > 0 )
        trk.session->tel.set_tcp_event(EVENT_DATA_AFTER_RST_RCVD);

    return default_state_action(tsd, trk);
}

bool TcpStateFinWait1::check_for_window_slam(TcpSegmentDescriptor& tsd, TcpStreamTracker& trk)
{
    DebugFormat(DEBUG_STREAM_STATE, "tsd.ack %X >= listener->snd_nxt %X\n",
        tsd.get_seg_ack(), trk.get_snd_nxt());

    if ( SEQ_EQ(tsd.get_seg_ack(), trk.get_snd_nxt() ) )
    {
        if ( (trk.normalizer->get_os_policy() == StreamPolicy::OS_WINDOWS)
            && (tsd.get_seg_wnd() == 0))
        {
            trk.session->tel.set_tcp_event(EVENT_WINDOW_SLAM);
            inc_tcp_discards();

            if (trk.normalizer->packet_dropper(tsd, NORM_TCP_BLOCK))
            {
                trk.session->set_pkt_action_flag(ACTION_BAD_PKT);
                return false;
            }
        }

        trk.set_tcp_state(TcpStreamTracker::TCP_FIN_WAIT2);
    }

    return true;
}

bool TcpStateFinWait1::do_pre_sm_packet_actions(TcpSegmentDescriptor& tsd, TcpStreamTracker& trk)
{
    return trk.session->validate_packet_established_session(tsd);
}

bool TcpStateFinWait1::do_post_sm_packet_actions(TcpSegmentDescriptor& tsd, TcpStreamTracker& trk)
{
    trk.session->update_paws_timestamps(tsd);
    trk.session->check_for_window_slam(tsd);
    return true;
}

