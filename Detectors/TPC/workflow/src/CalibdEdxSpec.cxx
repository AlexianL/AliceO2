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

/// \file CalibdEdxSpec.cxx
/// \brief Workflow for time based dE/dx calibration.
/// \author Thiago Badaró <thiago.saramela@usp.br>

#include "TPCWorkflow/CalibdEdxSpec.h"

// o2 includes
#include "CCDB/CcdbApi.h"
#include "CCDB/CcdbObjectInfo.h"
// #include "CommonUtils/NameConf.h"
#include "DataFormatsTPC/TrackTPC.h"
// #include "DataFormatsParameters/GRPObject.h"
#include "DetectorsCalibration/Utils.h"
#include "Framework/Task.h"
#include "Framework/DataProcessorSpec.h"
#include "Framework/ConfigParamRegistry.h"
#include "Framework/CCDBParamSpec.h"
#include "GPUO2InterfaceConfigurableParam.h"
#include "TPCCalibration/CalibdEdx.h"
#include "TPCWorkflow/ProcessingHelpers.h"
#include "TPCBase/CDBTypes.h"
#include "TPCBase/Utils.h"
#include "DetectorsBase/GRPGeomHelper.h"

using namespace o2::framework;

namespace o2::tpc
{

class CalibdEdxDevice : public Task
{
 public:
  CalibdEdxDevice(std::shared_ptr<o2::base::GRPGeomRequest> req, const o2::base::Propagator::MatCorrType matType) : mCCDBRequest(req), mMatType(matType) {}

  void init(framework::InitContext& ic) final
  {
    o2::base::GRPGeomHelper::instance().setRequest(mCCDBRequest);
    const auto minEntriesSector = ic.options().get<int>("min-entries-sector");
    const auto minEntries1D = ic.options().get<int>("min-entries-1d");
    const auto minEntries2D = ic.options().get<int>("min-entries-2d");
    const auto fitPasses = ic.options().get<int>("fit-passes");
    const auto fitThreshold = ic.options().get<float>("fit-threshold");
    const auto fitThresholdLowFactor = ic.options().get<float>("fit-threshold-low-factor");

    const auto dEdxBins = ic.options().get<int>("dedxbins");
    const auto mindEdx = ic.options().get<float>("min-dedx");
    const auto maxdEdx = ic.options().get<float>("max-dedx");
    const auto angularBins = ic.options().get<int>("angularbins");
    const auto fitSnp = ic.options().get<bool>("fit-snp");
    mMakeGaussianFits = !ic.options().get<bool>("disable-gaussian-fits");

    mDumpToFile = ic.options().get<int>("file-dump");

    mCalib = std::make_unique<CalibdEdx>(dEdxBins, mindEdx, maxdEdx, angularBins, fitSnp);
    mCalib->setApplyCuts(false);
    mCalib->setSectorFitThreshold(minEntriesSector);
    mCalib->set1DFitThreshold(minEntries1D);
    mCalib->set2DFitThreshold(minEntries2D);
    mCalib->setElectronCut(fitThreshold, fitPasses, fitThresholdLowFactor);
    mCalib->setMaterialType(mMatType);

    mCustomdEdxFileName = o2::gpu::GPUConfigurableParamGPUSettingsO2::Instance().dEdxCorrFile;
    mDisableTimeGain = o2::gpu::GPUConfigurableParamGPUSettingsO2::Instance().dEdxDisableResidualGain;

    if (mDisableTimeGain) {
      LOGP(info, "TimeGain correction was disabled via GPU_global.dEdxDisableResidualGain=1");
    }

    if (!mDisableTimeGain && !mCustomdEdxFileName.empty()) {
      std::unique_ptr<TFile> fdEdxCustom(TFile::Open(mCustomdEdxFileName.data()));
      if (!fdEdxCustom || !fdEdxCustom->IsOpen() || fdEdxCustom->IsZombie()) {
        LOGP(error, "Could not open custom TimeGain file {}", mCustomdEdxFileName);
      } else {
        const auto timeGain = fdEdxCustom->Get<o2::tpc::CalibdEdxCorrection>("CalibdEdxCorrection");
        if (!timeGain) {
          LOGP(error, "Could not load 'CalibdEdxCorrection' from file {}", mCustomdEdxFileName);
        } else {
          const auto meanParamTot = timeGain->getMeanParams(ChargeType::Tot);
          LOGP(info, "Loaded custom TimeGain from file {} with {} dimensions and mean qTot Params {}", mCustomdEdxFileName, timeGain->getDims(), utils::elementsToString(meanParamTot));
          mCalib->setCalibrationInput(*timeGain);
        }
      }
    }
  }

  void finaliseCCDB(o2::framework::ConcreteDataMatcher& matcher, void* obj) final
  {
    if (o2::base::GRPGeomHelper::instance().finaliseCCDB(matcher, obj)) {
      return;
    }
    if ((mDisableTimeGain == 0) && mCustomdEdxFileName.empty() && (matcher == ConcreteDataMatcher("TPC", "TIMEGAIN", 0))) {
      mCalib->setCalibrationInput(*(o2::tpc::CalibdEdxCorrection*)obj);
      const auto meanParamTot = mCalib->getCalibrationInput().getMeanParams(ChargeType::Tot);
      LOGP(info, "Updating TimeGain with {} dimensions and mean qTot Params {}", mCalib->getCalibrationInput().getDims(), utils::elementsToString(meanParamTot));
      return;
    }
  }

  void run(ProcessingContext& pc) final
  {
    o2::base::GRPGeomHelper::instance().checkUpdates(pc);
    checkUpdates(pc);
    const auto tfcounter = o2::header::get<DataProcessingHeader*>(pc.inputs().get("tracks").header)->startTime;
    const auto tracks = pc.inputs().get<gsl::span<TrackTPC>>("tracks");

    LOGP(detail, "Processing TF {} with {} tracks", tfcounter, tracks.size());
    mCalib->fill(tracks);

    // store run number and CCDB time only once
    if ((mTimeStampStart == 0) || (pc.services().get<o2::framework::TimingInfo>().timeslice == 0)) {
      mRunNumber = processing_helpers::getRunNumber(pc);
      mTimeStampStart = processing_helpers::getTimeStamp(pc, o2::base::GRPGeomHelper::instance().getOrbitResetTimeMS());
      LOGP(info, "Setting start time stamp for writing to CCDB to {}", mTimeStampStart);
    }
  }

  void endOfStream(EndOfStreamContext& eos) final
  {
    LOGP(info, "Finalizing calibration");
    mCalib->finalize(mMakeGaussianFits);
    mCalib->print();
    sendOutput(eos.outputs());

    if (mDumpToFile) {
      mCalib->dumpToFile("calibdEdx_Obj.root", "calib");
      mCalib->getCalib().writeToFile("calibdEdx.root");
      if (mDumpToFile > 1) {
        mCalib->writeTTree("calibdEdx.histo.tree.root");
      }
    }
  }

 private:
  void sendOutput(DataAllocator& output)
  {
    const auto& corr = mCalib->getCalib();
    o2::ccdb::CcdbObjectInfo info(CDBTypeMap.at(CDBType::CalTimeGain), std::string{}, std::string{}, std::map<std::string, std::string>{{"runNumber", std::to_string(mRunNumber)}}, mTimeStampStart, o2::ccdb::CcdbObjectInfo::INFINITE_TIMESTAMP);
    auto image = o2::ccdb::CcdbApi::createObjectImage(&corr, &info);
    LOGP(info, "Sending object {} / {} of size {} bytes, valid for {} : {} ", info.getPath(), info.getFileName(), image->size(), info.getStartValidityTimestamp(), info.getEndValidityTimestamp());
    output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBPayload, "TPC_CalibdEdx", 0}, *image.get());
    output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBWrapper, "TPC_CalibdEdx", 0}, info);
  }

  void checkUpdates(ProcessingContext& pc) const
  {
    if (pc.inputs().isValid("tpctimegain")) {
      pc.inputs().get<o2::tpc::CalibdEdxCorrection*>("tpctimegain");
    } else {
      return;
    }
  }

  std::shared_ptr<o2::base::GRPGeomRequest> mCCDBRequest;
  const o2::base::Propagator::MatCorrType mMatType{};
  int mDumpToFile{};
  uint64_t mRunNumber{0};      ///< processed run number
  uint64_t mTimeStampStart{0}; ///< time stamp for first TF for CCDB output
  std::unique_ptr<CalibdEdx> mCalib;
  bool mMakeGaussianFits{true};      ///< make gaussian fits or take the mean
  bool mDisableTimeGain{false};      ///< if time gain is disabled via GPU_global.dEdxDisableResidualGain=1
  std::string mCustomdEdxFileName{}; ///< name of the custom dE/dx file configured via GPU_global.dEdxCorrFile
};

DataProcessorSpec getCalibdEdxSpec(const o2::base::Propagator::MatCorrType matType)
{
  const bool enableAskMatLUT = matType == o2::base::Propagator::MatCorrType::USEMatCorrLUT;
  std::vector<OutputSpec> outputs;
  outputs.emplace_back(ConcreteDataTypeMatcher{o2::calibration::Utils::gDataOriginCDBPayload, "TPC_CalibdEdx"}, Lifetime::Sporadic);
  outputs.emplace_back(ConcreteDataTypeMatcher{o2::calibration::Utils::gDataOriginCDBWrapper, "TPC_CalibdEdx"}, Lifetime::Sporadic);
  std::vector<InputSpec> inputs{{"tracks", "TPC", "MIPS", Lifetime::Sporadic}};
  inputs.emplace_back("tpctimegain", "TPC", "TIMEGAIN", 0, Lifetime::Condition, ccdbParamSpec(o2::tpc::CDBTypeMap.at(o2::tpc::CDBType::CalTimeGain), {}, 1)); // time-dependent

  auto ccdbRequest = std::make_shared<o2::base::GRPGeomRequest>(true,                           // orbitResetTime
                                                                false,                          // GRPECS=true
                                                                false,                          // GRPLHCIF
                                                                true,                           // GRPMagField
                                                                enableAskMatLUT,                // askMatLUT
                                                                o2::base::GRPGeomRequest::None, // geometry
                                                                inputs,
                                                                true,
                                                                true);

  return DataProcessorSpec{
    "tpc-calib-dEdx",
    inputs,
    outputs,
    adaptFromTask<CalibdEdxDevice>(ccdbRequest, matType),
    Options{
      {"min-entries-sector", VariantType::Int, 1000, {"min entries per GEM stack to enable sector by sector correction. Below this value we only perform one fit per ROC type (IROC, OROC1, ...; no side nor sector information)."}},
      {"min-entries-1d", VariantType::Int, 10000, {"minimum entries per stack to fit 1D correction"}},
      {"min-entries-2d", VariantType::Int, 50000, {"minimum entries per stack to fit 2D correction"}},
      {"fit-passes", VariantType::Int, 3, {"number of fit iterations"}},
      {"fit-threshold", VariantType::Float, 0.2f, {"dEdx width around the MIP peak used in the fit"}},
      {"fit-threshold-low-factor", VariantType::Float, 1.5f, {"factor for low dEdx width around the MIP peak used in the fit"}},

      {"dedxbins", VariantType::Int, 70, {"number of dEdx bins"}},
      {"min-dedx", VariantType::Float, 10.0f, {"minimum value for dEdx histograms"}},
      {"max-dedx", VariantType::Float, 90.0f, {"maximum value for dEdx histograms"}},
      {"angularbins", VariantType::Int, 36, {"number of angular bins: Tgl and Snp"}},
      {"fit-snp", VariantType::Bool, false, {"enable Snp correction"}},
      {"disable-gaussian-fits", VariantType::Bool, false, {"disable calibration with gaussian fits and use mean instead"}},

      {"file-dump", VariantType::Int, 0, {"directly dump calibration to file"}}}};
}

} // namespace o2::tpc
