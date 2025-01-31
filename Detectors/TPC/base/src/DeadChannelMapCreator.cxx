// Copyright 2019-2020 CERN and copyright holders of ALICE O2.
// See https://alice-o2.web.cern.ch/copyright for details of the copyright holders.
// All rights not expressly granted are reserved.
//
// This software is distributed under the terms of the GNU General Public
// License v3 (GPL Version 3), copied verbatim in the file "COPYING".
//
// In applying this license CERN does not waive the privileges and immunities
// granted to it by virtue of its status as an Intergovernmental Organization
// or submit itself to any jurisdiction.

#include <map>
#include <string>
#include <type_traits>
#include "CommonUtils/NameConf.h"
#include "Framework/Logger.h"
#include "TPCBase/DeadChannelMapCreator.h"
#include "TPCBase/Painter.h"

using namespace o2::tpc;

void DeadChannelMapCreator::init(std::string_view url)
{
  mCCDBApi.init(url.empty() ? o2::base::NameConf::getCCDBServer() : url.data());
}

//______________________________________________________________________________
void DeadChannelMapCreator::load(long timeStampOrRun)
{
  timeStampOrRun = getTimeStamp(timeStampOrRun);

  loadFEEConfigViaRunInfo(timeStampOrRun);
  loadIDCPadFlags(timeStampOrRun);
  finalizeDeadChannelMap();
}

//______________________________________________________________________________
void DeadChannelMapCreator::loadFEEConfigViaRunInfoTS(long timeStamp)
{
  // it seems the object validity for the TPC RunInfo is not correct. For safety add 1min
  // since it has run validity this should be fine
  timeStamp += 60000;
  if (mObjectValidity[CDBType::ConfigRunInfo].isValid(timeStamp)) {
    return;
  }
  loadFEEConfig(timeStamp);
}

//______________________________________________________________________________
void DeadChannelMapCreator::loadFEEConfigViaRunInfo(long timeStampOrRun)
{
  loadFEEConfigViaRunInfoTS(getTimeStamp(timeStampOrRun));
}

//______________________________________________________________________________
void DeadChannelMapCreator::loadFEEConfig(long timeStamp)
{
  std::map<std::string, std::string> meta;
  mFEEConfig.reset(mCCDBApi.retrieveFromTFileAny<o2::tpc::FEEConfig>(CDBTypeMap.at(CDBType::ConfigRunInfo), {}, timeStamp, &meta));
  const long tag = std::stol(meta.at("Tag"));
  if (!mFEEConfig) {
    LOGP(error, "Could not load {}/{}, createdNotAfter: {}", CDBTypeMap.at(CDBType::ConfigFEE), tag, timeStamp);
    return;
  }
  LOGP(info, "Using FEE config for Tag {}, ETag {}, Last-Modified {}", meta.at("Valid-From"), meta.at("ETag"), meta.at("Last-Modified"));
  mDeadChannelMapFEE = mFEEConfig->getDeadChannelMap();
}

//______________________________________________________________________________
void DeadChannelMapCreator::loadIDCPadFlags(long timeStampOrRun)
{
  // TODO: Implement validity interval handling
  timeStampOrRun = getTimeStamp(timeStampOrRun);
  if (mObjectValidity[CDBType::CalIDCPadStatusMapA].isValid(timeStampOrRun)) {
    return;
  }

  std::map<std::string, std::string> meta;
  auto status = mCCDBApi.retrieveFromTFileAny<o2::tpc::CalDet<o2::tpc::PadFlags>>(CDBTypeMap.at(CDBType::CalIDCPadStatusMapA), {}, timeStampOrRun, &meta);
  mObjectValidity[CDBType::CalIDCPadStatusMapA].startvalidity = std::stol(meta.at("Valid-From"));
  mObjectValidity[CDBType::CalIDCPadStatusMapA].endvalidity = std::stol(meta.at("Valid-Until"));
  if (!status) {
    LOGP(error, "Could not load {}/{}", CDBTypeMap.at(CDBType::CalIDCPadStatusMapA), timeStampOrRun);
    return;
  }
  setDeadChannelMapIDCPadStatus(*status);
  mPadStatusMap.reset(status);
}

//______________________________________________________________________________
void DeadChannelMapCreator::setDeadChannelMapIDCPadStatus(const CalDetFlag_t& padStatusMap, PadFlags mask)
{
  mDeadChannelMapIDC = false;
  const auto& mapper = o2::tpc::Mapper::instance();
  for (size_t iROC = 0; iROC < mDeadChannelMapIDC.getData().size(); ++iROC) {
    auto& rocDeadMap = mDeadChannelMapIDC.getCalArray(iROC);
    o2::tpc::ROC roc(iROC);
    for (int iRow = 0; iRow < mapper.getNumberOfRowsROC(roc); ++iRow) {
      for (int iPad = 0; iPad < mapper.getNumberOfPadsInRowROC(roc, iRow); ++iPad) {
        if (std::underlying_type_t<PadFlags>(padStatusMap.getValue(roc, iRow, iPad)) & std::underlying_type_t<PadFlags>(mask)) {
          mDeadChannelMapIDC.getCalArray(iROC).setValue(iRow, iPad, true);
        }
      }
    }
  }
}

//______________________________________________________________________________
void DeadChannelMapCreator::finalizeDeadChannelMap()
{
  mDeadChannelMap = false; // reset map with false
  if (useSource(SourcesDeadMap::IDCPadStatus)) {
    mDeadChannelMap += mDeadChannelMapIDC;
  }
  if (useSource(SourcesDeadMap::FEEConfig)) {
    mDeadChannelMap += mDeadChannelMapFEE;
  }
}

//______________________________________________________________________________
void DeadChannelMapCreator::drawDeadChannelMapIDC()
{
  painter::makeSummaryCanvases(mDeadChannelMapIDC);
}

//______________________________________________________________________________
void DeadChannelMapCreator::drawDeadChannelMapFEE()
{
  painter::makeSummaryCanvases(mDeadChannelMapFEE);
}

//______________________________________________________________________________
void DeadChannelMapCreator::drawDeadChannelMap()
{
  painter::makeSummaryCanvases(mDeadChannelMap);
}
