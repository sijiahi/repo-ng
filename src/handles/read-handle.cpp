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

#include "read-handle.hpp"
#include "repo.hpp"

#include <ndn-cxx/util/logger.hpp>

namespace repo {

NDN_LOG_INIT(repo.ReadHandle);

ReadHandle::ReadHandle(Face& face, RepoStorage& storageHandle, size_t prefixSubsetLength)
  : m_prefixSubsetLength(prefixSubsetLength)
  , m_face(face)
  , m_storageHandle(storageHandle)
{
  connectAutoListen();
}

void
ReadHandle::connectAutoListen()
{
  // Connect a RepoStorage's signals to the read handle
  if (m_prefixSubsetLength != RepoConfig::DISABLED_SUBSET_LENGTH) {
    afterDataInsertionConnection = m_storageHandle.afterDataInsertion.connect(
      [this] (const Name& prefix) {
        onDataInserted(prefix);
      });
    afterDataDeletionConnection = m_storageHandle.afterDataDeletion.connect(
      [this] (const Name& prefix) {
        onDataDeleted(prefix);
      });
  }
}

void
ReadHandle::onInterest(const Name& prefix, const Interest& interest)
{
  std::cout<<"Received Interest " <<prefix <<"exact name"<<interest.getName()<<std::endl;
  NDN_LOG_DEBUG("Received Interest " << interest.getName());
  std::shared_ptr<ndn::Data> data = m_storageHandle.readData(interest);
  if (data != nullptr) {
    NDN_LOG_DEBUG("Put Data: " << *data);
    std::cout<<"Put Data: " << *data<<std::endl;
    m_face.put(*data);
  }   
  else {
    std::cout<<"No data for " << interest.getName()<<std::endl;
    NDN_LOG_DEBUG("No data for " << interest.getName());
  }
}

void
ReadHandle::onRegisterFailed(const Name& prefix, const std::string& reason)
{
  NDN_LOG_ERROR("ERROR: Failed to register prefix in local hub's daemon");
  m_face.shutdown();
}

void
ReadHandle::listen(const Name& prefix)
{
  std::cout<<"Filtering interest for prefix: "<<prefix<<std::endl;
  ndn::InterestFilter filter(prefix);
  m_face.setInterestFilter(filter,
                           std::bind(&ReadHandle::onInterest, this, _1, _2),
                           std::bind(&ReadHandle::onRegisterFailed, this, _1, _2));
}

void
ReadHandle::onDataDeleted(const Name& name)
{
  // We add one here to account for the implicit digest at the end,
  // which is what we get from the underlying storage when deleting.
  Name prefix = name.getPrefix(-(m_prefixSubsetLength + 1));
  auto check = m_insertedDataPrefixes.find(prefix);
  if (check != m_insertedDataPrefixes.end()) {
    if (--(check->second.useCount) <= 0) {
      check->second.hdl.unregister();
      m_insertedDataPrefixes.erase(prefix);
    }
  }
}

void
ReadHandle::onDataInserted(const Name& name)
{
  // Note: We want to save the prefix that we register exactly, not the
  // name that provoked the registration
  Name prefixToRegister = name.getPrefix(m_prefixSubsetLength);
  ndn::InterestFilter filter(prefixToRegister);
  auto check = m_insertedDataPrefixes.find(prefixToRegister);
  if (check == m_insertedDataPrefixes.end()) {
    // Because of stack lifetime problems, we assume here that the
    // prefix registration will be successful, and we add the registered
    // prefix to our list. This is because, if we fail, we shut
    // everything down, anyway. If registration failures are ever
    // considered to be recoverable, we would need to make this
    // atomic.
    std::cout<<"Filter Prefix"<<filter<<"name"<<name<<"prefixtoregister"<<prefixToRegister<<"m_prefixSubsetLength"<<m_prefixSubsetLength<<std::endl;
    auto hdl = m_face.setInterestFilter(filter,
      [this] (const ndn::InterestFilter& filter, const Interest& interest) {
        // Implicit conversion to Name of filter
        onInterest(filter, interest);
      },
      [] (const Name&) {},
      [this] (const Name& prefix, const std::string& reason) {
        onRegisterFailed(prefix, reason);
      });
    RegisteredDataPrefix registeredPrefix{hdl, 1};
    // Newly registered prefix
    m_insertedDataPrefixes.emplace(std::make_pair(prefixToRegister, registeredPrefix));
  }
  else {
    check->second.useCount++;
  }
}

} // namespace repo
