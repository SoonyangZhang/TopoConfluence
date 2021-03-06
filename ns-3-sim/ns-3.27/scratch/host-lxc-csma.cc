/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation;
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

// Network topology
//
//  +----------+
//  | external |
//  |  Linux   |
//  |   Host   |
//  |          |
//  | "mytap"  |
//  +----------+
//       |           n0               n4
//       |       +--------+     +------------+
//       +-------|  tap   |     |            |
//               | bridge |     |            |
//               +--------+     +------------+
//               |  Wifi  |-----| P2P | CSMA |                          
//               +--------+     +-----+------+                      
//                   |       ^           |                          
//                 ((*))     |           |                          
//                        P2P 10.1.2     |                         
//                 ((*))                 |    n5 ------------ "tap2", Linux container, 10.1.3.2      
//                   |                   |     | 
//                  n1                   ========
//                     Wifi 10.1.1                CSMA LAN 10.1.3
//
// The CSMA device on node zero is:  10.1.1.1
// The CSMA device on node one is:   10.1.1.2
// The P2P device on node three is:  10.1.2.1
// The P2P device on node four is:   10.1.2.2
// The CSMA device on node four is:  10.1.3.1
// The CSMA device on node five is:  10.1.3.2
//
// Some simple things to do:
//
// 1) Ping one of the simulated nodes on the left side of the topology.
//
//    ./waf --run tap-wifi-dumbbell&
//    ping 10.1.1.3
//
// 2) Configure a route in the linux host and ping once of the nodes on the 
//    right, across the point-to-point link.  You will see relatively large
//    delays due to CBR background traffic on the point-to-point (see next
//    item).
//
//    ./waf --run tap-wifi-dumbbell&
//    sudo route add -net 10.1.3.0 netmask 255.255.255.0 dev thetap gw 10.1.1.2
//    ping 10.1.3.4
//
//    Take a look at the pcap traces and note that the timing reflects the 
//    addition of the significant delay and low bandwidth configured on the 
//    point-to-point link along with the high traffic.
//
// 3) Fiddle with the background CBR traffic across the point-to-point 
//    link and watch the ping timing change.  The OnOffApplication "DataRate"
//    attribute defaults to 500kb/s and the "PacketSize" Attribute defaults
//    to 512.  The point-to-point "DataRate" is set to 512kb/s in the script,
//    so in the default case, the link is pretty full.  This should be 
//    reflected in large delays seen by ping.  You can crank down the CBR 
//    traffic data rate and watch the ping timing change dramatically.
//
//    ./waf --run "tap-wifi-dumbbell --ns3::OnOffApplication::DataRate=100kb/s"&
//    sudo route add -net 10.1.3.0 netmask 255.255.255.0 dev thetap gw 10.1.1.2
//    ping 10.1.3.4
//
// 4) Try to run this in UseBridge mode.  This allows you to bridge an ns-3
//    simulation to an existing pre-configured bridge.  This uses tap devices
//    just for illustration, you can create your own bridge if you want.
//
//    sudo tunctl -t mytap1
//    sudo ifconfig mytap1 0.0.0.0 promisc up
//    sudo tunctl -t mytap2
//    sudo ifconfig mytap2 0.0.0.0 promisc up
//    sudo brctl addbr mybridge
//    sudo brctl addif mybridge mytap1
//    sudo brctl addif mybridge mytap2
//    sudo ifconfig mybridge 10.1.1.5 netmask 255.255.255.0 up
//    ./waf --run "tap-wifi-dumbbell --mode=UseBridge --tapName=mytap2"&
//    ping 10.1.1.3

// host: sudo route add -net 10.1.3.0 gw 10.1.1.2 netmask 255.255.255.0 dev tap1
// container: route add -net 10.1.1.0 gw 10.1.3.1 netmask 255.255.255.0 dev eth0
    // 

#include <iostream>
#include <fstream>

#include "ns3/core-module.h"
#include "ns3/network-module.h"
#include "ns3/mobility-module.h"
#include "ns3/point-to-point-module.h"
#include "ns3/wifi-module.h"
#include "ns3/internet-module.h"
#include "ns3/csma-module.h"
#include "ns3/applications-module.h"
#include "ns3/ipv4-global-routing-helper.h"
#include "ns3/tap-bridge-module.h"

using namespace ns3;

NS_LOG_COMPONENT_DEFINE ("HostLxcCsma");

int 
main (int argc, char *argv[])
{
  std::string mode = "ConfigureLocal";              // only for tap1
  std::string tapName1 = "tap-left", tapName2 = "tap-right";
  double tStop = 600;

  CommandLine cmd;
  cmd.AddValue ("mode", "Mode setting of TapBridge", mode);
  cmd.AddValue ("tStop", "Time of the simulation", tStop);
  cmd.AddValue ("tapName1", "Name of the OS tap device (left)", tapName1);
  cmd.AddValue ("tapName2", "Name of the OS tap device (right)", tapName2);
  cmd.Parse (argc, argv);

  GlobalValue::Bind ("SimulatorImplementationType", StringValue ("ns3::RealtimeSimulatorImpl"));
  GlobalValue::Bind ("ChecksumEnabled", BooleanValue (true));
  LogComponentEnable("HostLxcCsma", LOG_INFO);

  //
  // The topology has a Wifi network of four nodes on the left side.  We'll make
  // node zero the AP and have the other three will be the STAs.
  //
  NodeContainer nodesLeft;
  nodesLeft.Create (2);

  CsmaHelper csma;
  csma.SetChannelAttribute("DataRate", StringValue("10Gbps"));
  // csma.SetChannelAttribute("Delay", StringValue("2ms"));
  // NS_LOG_INFO("CSMA rate: 1Gbps, delay: 2ms");

  NetDeviceContainer devicesLeft = csma.Install (nodesLeft);

  InternetStackHelper internetLeft;
  internetLeft.Install (nodesLeft);

  Ipv4AddressHelper ipv4Left;
  ipv4Left.SetBase ("10.1.1.0", "255.255.255.0");
  Ipv4InterfaceContainer interfacesLeft = ipv4Left.Assign (devicesLeft);

  TapBridgeHelper tapBridge (interfacesLeft.GetAddress (1));        // why this address? --> Doc: gateway address!!!
  tapBridge.SetAttribute ("Mode", StringValue (mode));
  tapBridge.SetAttribute ("DeviceName", StringValue (tapName1));
  tapBridge.Install (nodesLeft.Get (0), devicesLeft.Get (0));
  NS_LOG_INFO("Left tap gateway: " << interfacesLeft.GetAddress (1));

  //
  // Now, create the right side.
  //
  NodeContainer nodesRight;
  nodesRight.Create (2);

  CsmaHelper csmaRight;
  csmaRight.SetChannelAttribute ("DataRate", StringValue ("10Gbps"));
  // csmaRight.SetChannelAttribute ("Delay", TimeValue (MilliSeconds (2)));

  NetDeviceContainer devicesRight = csmaRight.Install (nodesRight);

  InternetStackHelper internetRight;
  internetRight.Install (nodesRight);

  Ipv4AddressHelper ipv4Right;
  ipv4Right.SetBase ("10.1.3.0", "255.255.255.0");
  Ipv4InterfaceContainer interfacesRight = ipv4Right.Assign (devicesRight);

  // add a tap bridge to node 7
  TapBridgeHelper tapBridge2(interfacesRight.GetAddress(0));
  tapBridge.SetAttribute ("Mode", StringValue ("UseBridge"));
  tapBridge.SetAttribute ("DeviceName", StringValue (tapName2));
  tapBridge.Install (nodesRight.Get (1), devicesRight.Get (1));         // install on node 5: 10.1.3.2
  NS_LOG_INFO("Right tap gateway: " << interfacesRight.GetAddress(0));

  //
  // Stick in the point-to-point line between the sides.
  //
  PointToPointHelper p2p;
  p2p.SetDeviceAttribute ("DataRate", StringValue ("10Gbps"));
  p2p.SetChannelAttribute ("Delay", StringValue ("5ms"));

  NodeContainer nodes = NodeContainer (nodesLeft.Get (1), nodesRight.Get (0));
  NetDeviceContainer devices = p2p.Install (nodes);

  Ipv4AddressHelper ipv4;
  ipv4.SetBase ("10.1.2.0", "255.255.255.192");
  Ipv4InterfaceContainer interfaces = ipv4.Assign (devices);

//   //
//   // Simulate some CBR traffic over the point-to-point link
//   //
//   uint16_t port = 9;   // Discard port (RFC 863)
//   OnOffHelper onoff ("ns3::UdpSocketFactory", InetSocketAddress (interfaces.GetAddress (1), port));
//   onoff.SetConstantRate (DataRate ("1kb/s"));

//   ApplicationContainer apps = onoff.Install (nodesLeft.Get (3));
//   apps.Start (Seconds (1.0));

//   // Create a packet sink to receive these packets
//   PacketSinkHelper sink ("ns3::UdpSocketFactory", InetSocketAddress (Ipv4Address::GetAny (), port));

//   apps = sink.Install (nodesRight.Get (0));
//   apps.Start (Seconds (1.0));

  p2p.EnablePcapAll ("host-lxc");
  csmaRight.EnablePcapAll ("host-lxc-csma", false);
  Ipv4GlobalRoutingHelper::PopulateRoutingTables ();

  Simulator::Stop (Seconds (tStop));
  Simulator::Run ();
  Simulator::Destroy ();
}
