/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014-2019, Regents of the University of California.
 *
 * This file is part of NDN repo-ng (Next generation of NDN repository).
 * See AUTHORS.md for complete list of repo-ng authors and contributors.
 *
 * repo-ng is free software: you can redistribute it and/or modify it under the terms
 * of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * repo-ng is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 * PURPOSE.  See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * repo-ng, e.g., in COPYING.md file.  If not, see <http://www.gnu.org/licenses/>.
 */

#include "write-handle.hpp"

#include <ndn-cxx/util/logger.hpp>
#include <ndn-cxx/util/random.hpp>

namespace repo {

NDN_LOG_INIT(repo.WriteHandle);

static const int DEFAULT_CREDIT = 12;
static const bool DEFAULT_CANBE_PREFIX = false;
static const milliseconds MAX_TIMEOUT(60_s);
static const milliseconds NOEND_TIMEOUT(10000_ms);
static const milliseconds PROCESS_DELETE_TIME(10000_ms);
static const milliseconds DEFAULT_INTEREST_LIFETIME(4000_ms);

WriteHandle::WriteHandle(Face& face, RepoStorage& storageHandle, ndn::mgmt::Dispatcher& dispatcher,
                         Scheduler& scheduler, Validator& validator)
  : CommandBaseHandle(face, storageHandle, scheduler, validator)
  , m_validator(validator)
  , m_credit(DEFAULT_CREDIT)
  , m_canBePrefix(DEFAULT_CANBE_PREFIX)
  , m_maxTimeout(MAX_TIMEOUT)
  , m_noEndTimeout(NOEND_TIMEOUT)
  , m_interestLifetime(DEFAULT_INTEREST_LIFETIME)
{
  std::cout<<"Initializing Write Handle" << std::endl;
  NDN_LOG_DEBUG("Initializing Write Handle" << std::endl);
  dispatcher.addControlCommand<RepoCommandParameter>(ndn::PartialName("insert"),
    makeAuthorization(),
    std::bind(&WriteHandle::validateParameters<InsertCommand>, this, _1),
    std::bind(&WriteHandle::handleInsertCommand, this, _1, _2, _3, _4));

  dispatcher.addControlCommand<RepoCommandParameter>(ndn::PartialName("insertcheck"),
    makeAuthorization(),
    std::bind(&WriteHandle::validateParameters<InsertCheckCommand>, this, _1),
    std::bind(&WriteHandle::handleCheckCommand, this, _1, _2, _3, _4));
}

void
WriteHandle::deleteProcess(ProcessId processId)
{
  m_processes.erase(processId);
}

void
WriteHandle::handleInsertCommand(const Name& prefix, const Interest& interest,
                                 const ndn::mgmt::ControlParameters& parameter,
                                 const ndn::mgmt::CommandContinuation& done)
{
  std::cout<<"Handling Insert command for Interest: "<<interest<<std::endl;
  RepoCommandParameter* repoParameter =
    dynamic_cast<RepoCommandParameter*>(const_cast<ndn::mgmt::ControlParameters*>(&parameter));
  if (repoParameter->hasStartBlockId() || repoParameter->hasEndBlockId()) {
    std::cout<<"processSegmentedInsertCommand"<<repoParameter->hasStartBlockId()<<std::endl;
    processSegmentedInsertCommand(interest, *repoParameter, done);
  }
  else {
    std::cout<<"processSingleInsertCommand"<<std::endl;
    processSingleInsertCommand(interest, *repoParameter, done);
  }
  if (repoParameter->hasInterestLifetime())
    m_interestLifetime = repoParameter->getInterestLifetime();
}

void
WriteHandle::onData(const Interest& interest, const Data& data, ProcessId processId)
{
  m_validator.validate(data,
                       std::bind(&WriteHandle::onDataValidated, this, interest, _1, processId),
                       [](const Data& data, const ValidationError& error){NDN_LOG_ERROR("Error: " << error);});
}

void
WriteHandle::onDataValidated(const Interest& interest, const Data& data, ProcessId processId)
{
  if (m_processes.count(processId) == 0) {
    return;
  }

  ProcessInfo& process = m_processes[processId];
  RepoCommandResponse& response = process.response;

  if (response.getInsertNum() == 0) {
    storageHandle.insertData(data);
    response.setInsertNum(1);
  }

  deferredDeleteProcess(processId);
}

void
WriteHandle::onTimeout(const Interest& interest, ProcessId processId)
{
  NDN_LOG_DEBUG("Timeout" << std::endl);
  m_processes.erase(processId);
}

void
WriteHandle::processSingleInsertCommand(const Interest& interest, RepoCommandParameter& parameter,
                                        const ndn::mgmt::CommandContinuation& done)
{
  ProcessId processId = ndn::random::generateWord64();

  ProcessInfo& process = m_processes[processId];

  RepoCommandResponse& response = process.response;
  response.setCode(100);
  response.setProcessId(processId);
  response.setInsertNum(0);
  response.setBody(response.wireEncode());
  done(response);

  response.setCode(300);
  Interest fetchInterest(parameter.getName());
  fetchInterest.setCanBePrefix(m_canBePrefix);
  fetchInterest.setInterestLifetime(m_interestLifetime);
  std::cout<<"Single Insert, Fetch interest"<<fetchInterest.getName()<<std::endl;
  face.expressInterest(fetchInterest,
                       std::bind(&WriteHandle::onData, this, _1, _2, processId),
                       std::bind(&WriteHandle::onTimeout, this, _1, processId), // Nack
                       std::bind(&WriteHandle::onTimeout, this, _1, processId));
}

void
WriteHandle::segInit(ProcessId processId, const RepoCommandParameter& parameter)
{
  // use SegmentFetcher to send fetch interest.
  ProcessInfo& process = m_processes[processId];
  Name name = parameter.getName();
  SegmentNo startBlockId = parameter.getStartBlockId();

  uint64_t initialCredit = m_credit;

  if (parameter.hasEndBlockId()) {
    initialCredit =
      std::min(initialCredit, parameter.getEndBlockId() - parameter.getStartBlockId() + 1);
  }
  else {
    // set noEndTimeout timer
    process.noEndTime = ndn::time::steady_clock::now() +
                        m_noEndTimeout;
  }

  Name fetchName = name;
  SegmentNo segment = startBlockId;
  fetchName.appendSegment(segment);
  Interest interest(fetchName);

  ndn::util::SegmentFetcher::Options options;
  options.initCwnd = initialCredit;
  options.interestLifetime = m_interestLifetime;
  options.maxTimeout = m_maxTimeout;
  std::cout<<"Sending Fetch interest"<<interest.getName()<<std::endl;
  auto fetcher = ndn::util::SegmentFetcher::start(face, interest, m_validator, options);
  fetcher->onError.connect([] (uint32_t errorCode, const std::string& errorMsg)
                           {NDN_LOG_ERROR("Error: " << errorMsg);});
  fetcher->afterSegmentValidated.connect([this, &fetcher, processId] (const Data& data)
                                         {onSegmentData(*fetcher, data, processId);});
  fetcher->afterSegmentTimedOut.connect([this, &fetcher, processId] ()
                                        {onSegmentTimeout(*fetcher, processId);});
}

void
WriteHandle::onSegmentData(ndn::util::SegmentFetcher& fetcher, const Data& data, ProcessId processId)
{
  std::cout<<"received Fetched data"<<data<<"processId"<<processId<<std::endl;
  auto it = m_processes.find(processId);
  if (it == m_processes.end()) {
    fetcher.stop();
    return;
  }

  RepoCommandResponse& response = it->second.response;

  if (storageHandle.insertData(data)) {
    response.setInsertNum(response.getInsertNum() + 1);
  }
  if (data.getFinalBlock()){
    std::cout<<"Final"<<data.getFinalBlock()->toSegment()<<std::endl;
    response.setEndBlockId(data.getFinalBlock()->toSegment());
  }

  ProcessInfo& process = m_processes[processId];
  //read whether notime timeout
  if (!response.hasEndBlockId()) {
    ndn::time::steady_clock::TimePoint& noEndTime = process.noEndTime;
    ndn::time::steady_clock::TimePoint now = ndn::time::steady_clock::now();
    if (now > noEndTime) {
      NDN_LOG_DEBUG("noEndtimeout: " << processId);
      //StatusCode should be refreshed as 405
      response.setCode(405);
      //schedule a delete event
      deferredDeleteProcess(processId);
      fetcher.stop();
      return;
    }
  }
  //read whether this process has total ends, if ends, remove control info from the maps
  if (response.hasEndBlockId()) {
    uint64_t nSegments = response.getEndBlockId() - response.getStartBlockId() + 1;
    if (response.getInsertNum() >= nSegments) {
      //All the data has been inserted, StatusCode is refreshed as 200
      response.setCode(200);
      deferredDeleteProcess(processId);
      fetcher.stop();
      return;
    }
  }
}

void
WriteHandle::onSegmentTimeout(ndn::util::SegmentFetcher& fetcher, ProcessId processId)
{
  NDN_LOG_DEBUG("SegTimeout");
  if (m_processes.count(processId) == 0) {
    fetcher.stop();
    return;
  }
}

//Segmented Intert Command:
//
void
WriteHandle::processSegmentedInsertCommand(const Interest& interest, RepoCommandParameter& parameter,
                                           const ndn::mgmt::CommandContinuation& done)
{
  std::cout<<"parameter.hasEndBlockId()"<<parameter.hasEndBlockId()<<std::endl;
  if (parameter.hasEndBlockId()) {
    
    //normal fetch segment
    SegmentNo startBlockId = parameter.hasStartBlockId() ? parameter.getStartBlockId() : 0;
    std::cout<<"startBlockId"<<startBlockId<<std::endl;
    SegmentNo endBlockId = parameter.getEndBlockId();
    std::cout<<"endBlockId"<<endBlockId<<std::endl;
    if (startBlockId > endBlockId) {
      done(negativeReply("Malformed Command", 403));
      return;
    }

    ProcessId processId = ndn::random::generateWord64();
    std::cout<<"Tagged as Process: "<<processId<<std::endl;
    ProcessInfo& process = m_processes[processId];
    RepoCommandResponse& response = process.response;
    response.setCode(100);
    response.setProcessId(processId);
    response.setInsertNum(0);
    response.setStartBlockId(startBlockId);
    response.setEndBlockId(endBlockId);
    //std::cout<<"Prepared command response: "<<response<<std::endl;
    //response.wireEncode();
   
    response.setBody(response.wireEncode());
     done(response);
    std::cout<<"Response: "<<response.getBody()<<std::endl;
    //done(interest(response));
    

    //300 means data fetching is in progress
    response.setCode(300);

    segInit(processId, parameter);
  }
  else {
    //no EndBlockId, so fetch FinalBlockId in data, if timeout, stop
    ProcessId processId = ndn::random::generateWord64();
    ProcessInfo& process = m_processes[processId];
    RepoCommandResponse& response = process.response;
    response.setCode(100);
    response.setProcessId(processId);
    response.setInsertNum(0);
    response.setStartBlockId(parameter.getStartBlockId());
    response.setBody(response.wireEncode());
    done(response);

    //300 means data fetching is in progress
    response.setCode(300);

    segInit(processId, parameter);
  }
}

void
WriteHandle::handleCheckCommand(const Name& prefix, const Interest& interest,
                                const ndn::mgmt::ControlParameters& parameter,
                                const ndn::mgmt::CommandContinuation& done)
{
  const RepoCommandParameter& repoParameter = dynamic_cast<const RepoCommandParameter&>(parameter);

  //check whether this process exists
  ProcessId processId = repoParameter.getProcessId();
  if (m_processes.count(processId) == 0) {
    NDN_LOG_DEBUG("no such processId: " << processId);
    done(negativeReply("No such this process is in progress", 404));
    return;
  }

  ProcessInfo& process = m_processes[processId];

  RepoCommandResponse& response = process.response;

  //Check whether it is single data fetching
  if (!response.hasStartBlockId() && !response.hasEndBlockId()) {
    done(response);
    return;
  }

  //read if noEndtimeout
  if (!response.hasEndBlockId()) {
    extendNoEndTime(process);
    done(response);
    return;
  }
  else {
    done(response);
  }
}

void
WriteHandle::deferredDeleteProcess(ProcessId processId)
{
  scheduler.schedule(PROCESS_DELETE_TIME, [=] { deleteProcess(processId); });
}

void
WriteHandle::extendNoEndTime(ProcessInfo& process)
{
  auto& noEndTime = process.noEndTime;
  auto now = ndn::time::steady_clock::now();
  RepoCommandResponse& response = process.response;
  if (now > noEndTime) {
    response.setCode(405);
    return;
  }

  //extends noEndTime
  process.noEndTime = ndn::time::steady_clock::now() + m_noEndTimeout;
}

RepoCommandResponse
WriteHandle::negativeReply(std::string text, int statusCode)
{
  RepoCommandResponse response(statusCode, text);
  response.setBody(response.wireEncode());

  return response;
}

} // namespace repo