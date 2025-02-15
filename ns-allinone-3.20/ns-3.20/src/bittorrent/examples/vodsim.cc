/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2010-2013, 2015 ComSys, RWTH Aachen University
 *
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
 *
 * Authors: Rene Glebke (principal author), Alexander Hocks
 */

///////////////////////////////////////////////////////////////////////////////
// UNCOMMENT THIS FOR BITTORRENT EMULATION CAPABILITIES
// #define BITTORRENT_EMULATION
///////////////////////////////////////////////////////////////////////////////

#include "ns3/BitTorrentTracker.h"
#include "ns3/BitTorrentVideoClient.h"
#include "ns3/VODSimBriteTopologyHelper.h"
#include "ns3/GlobalMetricsGatherer.h"
#include "ns3/Story.h"

#include "ns3/application.h"
#include "ns3/application-container.h"
#include "ns3/core-module.h"
#include "ns3/realtime-simulator-impl.h"
#include "ns3/simulator.h"
#include "ns3/node.h"
#include "ns3/node-container.h"

#ifdef NS3_MPI
#include <mpi.h>
#include "ns3/mpi-interface.h"
#endif


//Immovable Rock
#include<queue>
#include"ns3/event-id.h"

using namespace ns3;
using namespace bittorrent;

NS_LOG_COMPONENT_DEFINE ("BitTorrentVoDSimulation");

void ShowTimePeriodic ()
{
  std::cout << "It is now " <<  Simulator::Now ().GetSeconds () << "s (" << GlobalMetricsGatherer::GetWallclockTime () << ")" << std::endl;
  Simulator::Schedule (Seconds (1.0), ShowTimePeriodic);
}

int main (int argc, char *argv[])
{
  // LogComponentEnable ("bittorrent::Peer", LOG_LEVEL_ALL);
  // LogComponentEnable ("bittorrent::PartSelectionStrategyBase", LOG_LEVEL_ALL);
  LogComponentEnable ("bittorrent::PeerConnectorStrategyBase", LOG_LEVEL_ALL);
  LogComponentEnable ("bittorrent::VODSimBriteTopologyHelper", LOG_LEVEL_ALL);

#ifdef NS3_MPI
  // MPI setup -->
  MpiInterface::Enable (&argc, &argv);
  GlobalValue::Bind ("SimulatorImplementationType", StringValue ("ns3::DistributedSimulatorImpl"));
  // <-- MPI setup
#endif

#ifdef BITTORRENT_EMULATION
  // Emulation setup (part 1) -->
  //  GlobalValue::Bind ("SimulatorImplementationType", StringValue ("ns3::RealtimeSimulatorImpl"));
  GlobalValue::Bind ("SimulatorImplementationType", StringValue ("ns3::SyncSimulatorImpl"));
  GlobalValue::Bind ("ChecksumEnabled", BooleanValue (true));
  // <-- Emulation setup (part 1)
#endif

  std::string storyFileName = "simulation";
  std::string replacements = "";
  uint32_t simulationDuration = 25000;
  bool enableLogging = false;
  std::string comment;
  CommandLine cmd;
  cmd.AddValue ("story", "Name of the story input file, without \".story\" ending. Expected to reside within the ns3 directory tree", storyFileName);
  cmd.AddValue ("replacements", "Variable replacements that shall take place while parsing the story input file, in format \"variable_1:value_1/variable2:value_2\".", replacements);
  cmd.AddValue ("duration", "Length of the simulation in seconds", simulationDuration);
  cmd.AddValue ("logging", "Full-scale logging (0 = off, 1 = on)", enableLogging);
  cmd.AddValue ("comment", "A debugging comment; may be used to identify your simulation run", comment);
  cmd.Parse (argc, argv);

  std::cout << "Setting up BitTorrent Video-on-Demand simulation..." << std::endl;

  NodeContainer routerNodes;
  NodeContainer btNodes;
  NodeContainer btTrackerNode;
  NodeContainer otherNodes;
  NodeContainer tapNodes;
  VODSimBriteTopologyHelper topologyHelper;
  ApplicationContainer nodeApplicationContainer;
  ApplicationContainer trackerApplicationContainer;

  std::cout << "Reading story file and registering simulation events..." << std::endl;

  Story* story = Story::GetInstance ();
  story->SetRouterNodeContainer (&routerNodes);
  story->SetBTNodeContainer (&btNodes);
  story->SetBTTrackerContainer (&btTrackerNode);
  story->SetOtherNodeContainer (&otherNodes);
  story->SetTapNodeContainer (&tapNodes);
  story->SetBriteTopologyHelper (&topologyHelper);
  story->SetBTNodeApplicationContainer (&nodeApplicationContainer);
  story->SetBTTrackerApplicationContainer (&trackerApplicationContainer);
  story->ParseReplacements (replacements);
  story->ReadAndScheduleStory (storyFileName + ".story", simulationDuration);

  std::cout << "Configuring network topology..." << std::endl;

#ifdef NS3_MPI
  topologyHelper.DistributeRouterNodesToMPISystems();
  topologyHelper.DistributeClientNodesToMPISystems();
#endif
  topologyHelper.EstablishNetworkOnLastTopology ();

  std::cout << "Configuring tracker for network topology..." << std::endl;

  BitTorrentTracker* btTracker = dynamic_cast<BitTorrentTracker*> (PeekPointer (btTrackerNode.Get (0)->GetApplication (0)));
  Ptr<Torrent> sharedTorrent = btTracker->AddTorrent (story->GetTorrentFolder (), story->GetTorrentFile ());
  btTracker->PrepareForManyClients (sharedTorrent, story->GetBTNodeCount () + 1);
  std::cout << "	Tracker announce URL: "<< btTracker->GetAnnounceURL () << std::endl;

  std::cout << "Configuring clients for network topology..." << std::endl;

  for (NodeContainer::Iterator it = btNodes.Begin (); it != btNodes.End (); ++it)
    {
#ifdef NS3_MPI
      if (PeekPointer (*it)->GetSystemId () == MpiInterface::GetSystemId ())
#endif
        {
          if (PeekPointer(*it)->GetNApplications() > 0) { // We assume the first application to be a BitTorrentClient here
              dynamic_cast<BitTorrentVideoClient*> (PeekPointer ((*it)->GetApplication (0)))->SetTorrent (PointerValue (sharedTorrent));
          }
        }
    }

  std::cout << "Configuring global metrics gatherer..." << std::endl;
  GlobalMetricsGatherer* gatherer = GlobalMetricsGatherer::GetInstance ();
  gatherer->SetFileNamePrefix (story->GetSimulationId (), story->GetLoggingToFile ());
  gatherer->RegisterWithApplications (nodeApplicationContainer);
  gatherer->AnnounceExternalClients (tapNodes.GetN ());
// copying event ids of new pool to reserve pool in GlobalMetricsGatherer object
  gatherer->ReservePool = story->newleechers; 
std::cout<< "size of the reserve pool is " << gatherer->ReservePool.size() << std::endl;
#ifndef BITTORRENT_EMULATION
  gatherer->SetStopFraction (1.0, 1.0);
#else
  // Emulation setup (part 2) -->
  gatherer->SetStopFraction (-1.0, 1.0);
  // <-- Emulation setup (part 2)
#endif

  // topologyHelper.WriteLastTopologyToGraphVizFile ("output/" + story->GetSimulationId () + ".dot");

  std::cout << "Starting BitTorrent Video-on-Demand simulation..." << std::endl;
  Simulator::ScheduleNow (ShowTimePeriodic);
  Simulator::Stop (Seconds (simulationDuration) + MilliSeconds (1));
  gatherer->WriteToFile ("simulation-started", GlobalMetricsGatherer::GetWallclockTime (), false);
  Simulator::Run ();
  Simulator::Destroy ();

  std::cout << GlobalMetricsGatherer::GetWallclockTime () << ": BitTorent Video-on-Demand simulation finished successfully :=)" << std::endl;
}

