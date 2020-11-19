/* -*- Mode:C++; c-file-style:"gnu"; indent-tabs-mode:nil; -*- */
/*
 * Copyright (c) 2014-2018, Regents of the University of California.
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

#ifndef REPO_HANDLES_COMMAND_BASE_HANDLE_HPP
#define REPO_HANDLES_COMMAND_BASE_HANDLE_HPP

#include "common.hpp"

#include "storage/repo-storage.hpp"
#include "repo-command-response.hpp"
#include "repo-command-parameter.hpp"
#include "repo-command.hpp"

#include <ndn-cxx/mgmt/dispatcher.hpp>

namespace repo {

class CommandBaseHandle
{
public:
  class Error : public std::runtime_error
  {
  public:
    explicit
    Error(const std::string& what)
      : std::runtime_error(what)
    {
    }
  };

public:
  CommandBaseHandle(Face& face, RepoStorage& storageHandle,
                    Scheduler& scheduler, Validator& validator);

  virtual
  ~CommandBaseHandle() = default;

  ndn::mgmt::Authorization
  makeAuthorization();

  template<typename T>
  bool
  validateParameters(const ndn::mgmt::ControlParameters& parameters)
  {
    std::cout<<"Validating Parameters"<<std::endl;
    const RepoCommandParameter* castParams =
      dynamic_cast<const RepoCommandParameter*>(&parameters);
    BOOST_ASSERT(castParams != nullptr);
    T command;
    try {
      command.validateRequest(*castParams);
    }
    catch (const RepoCommand::ArgumentError& ae) {
      std::cout<<" RepoCommand::ArgumentError :"<<ae.what()<<std::endl;
      return false;
    }
    return true;
  }

protected:
  Face& face;
  RepoStorage& storageHandle;
  Scheduler& scheduler;

private:
  Validator& m_validator;
};
} // namespace repo

#endif // REPO_HANDLES_COMMAND_BASE_HANDLE_HPP