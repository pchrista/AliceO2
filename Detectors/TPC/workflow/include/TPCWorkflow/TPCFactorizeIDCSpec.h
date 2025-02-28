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

/// \file TPCFactorizeGroupedIDCSpec.h
/// \brief TPC aggregation of grouped IDCs and factorization
/// \author Matthias Kleiner <mkleiner@ikf.uni-frankfurt.de>
/// \date Jun 25, 2021

#ifndef O2_TPCFACTORIZEIDCSPEC_H
#define O2_TPCFACTORIZEIDCSPEC_H

#include <vector>
#include <fmt/format.h>
#include <filesystem>
#include "Framework/Task.h"
#include "Framework/ControlService.h"
#include "Framework/Logger.h"
#include "Framework/DataProcessorSpec.h"
#include "Framework/DeviceSpec.h"
#include "Framework/DataTakingContext.h"
#include "DetectorsCommonDataFormats/FileMetaData.h"
#include "Headers/DataHeader.h"
#include "TPCCalibration/IDCFactorization.h"
#include "TPCCalibration/IDCAverageGroup.h"
#include "CCDB/CcdbApi.h"
#include "TPCWorkflow/TPCDistributeIDCSpec.h"
#include "TPCBase/CRU.h"
#include "CommonUtils/NameConf.h"
#include "TPCWorkflow/ProcessingHelpers.h"
#include "TPCBase/CDBInterface.h"
#include "DetectorsCalibration/Utils.h"

using namespace o2::framework;
using o2::header::gDataOriginTPC;
using namespace o2::tpc;

namespace o2::tpc
{

class TPCFactorizeIDCSpec : public o2::framework::Task
{
 public:
  TPCFactorizeIDCSpec(const std::vector<uint32_t>& crus, const unsigned int timeframes, const unsigned int timeframesDeltaIDC, std::array<unsigned char, Mapper::NREGIONS> groupPads,
                      std::array<unsigned char, Mapper::NREGIONS> groupRows, std::array<unsigned char, Mapper::NREGIONS> groupLastRowsThreshold,
                      std::array<unsigned char, Mapper::NREGIONS> groupLastPadsThreshold, const unsigned int groupPadsSectorEdges, const IDCDeltaCompression compression, const bool usePrecisetimeStamp, const bool sendOutputFFT, const bool sendCCDB, const int lane, const std::vector<o2::tpc::Side>& sides, const int nTFsBuffer, const bool processClusters)
    : mCRUs{crus}, mIDCFactorization{timeframes, timeframesDeltaIDC, crus}, mIDCGrouping{groupPads, groupRows, groupLastRowsThreshold, groupLastPadsThreshold, groupPadsSectorEdges}, mCompressionDeltaIDC{compression}, mUsePrecisetimeStamp{usePrecisetimeStamp}, mSendOutFFT{sendOutputFFT}, mSendOutCCDB{sendCCDB}, mLaneId{lane}, mSides{sides}, mNTFsBuffer{nTFsBuffer}, mProcessClusters{processClusters} {};

  void init(o2::framework::InitContext& ic) final
  {
    mUpdateGroupingPar = mLaneId == 0 ? !(ic.options().get<bool>("update-not-grouping-parameter")) : false;
    mIDCFactorization.setUsePadStatusMap(ic.options().get<bool>("enablePadStatusMap"));
    mEnableWritingPadStatusMap = ic.options().get<bool>("enableWritingPadStatusMap");
    mNOrbitsIDC = ic.options().get<int>("orbits-IDCs");
    mDumpIDC0 = ic.options().get<bool>("dump-IDC0");
    mDumpIDC1 = ic.options().get<bool>("dump-IDC1");
    mDumpIDCDelta = ic.options().get<bool>("dump-IDCDelta");
    mDumpIDCDeltaCalibData = ic.options().get<bool>("dump-IDCDelta-calib-data");
    mDumpIDCs = ic.options().get<bool>("dump-IDCs");
    mOffsetCCDB = ic.options().get<bool>("add-offset-for-CCDB-timestamp");
    mDisableIDCDelta = ic.options().get<bool>("disable-IDCDelta");
    mCalibFileDir = ic.options().get<std::string>("output-dir");
    if (mCalibFileDir != "/dev/null") {
      mCalibFileDir = o2::utils::Str::rectifyDirectory(mCalibFileDir);
    }
    mMetaFileDir = ic.options().get<std::string>("meta-output-dir");
    if (mMetaFileDir != "/dev/null") {
      mMetaFileDir = o2::utils::Str::rectifyDirectory(mMetaFileDir);
    }

    const std::string refGainMapFile = ic.options().get<std::string>("gainMapFile");
    if (!refGainMapFile.empty()) {
      LOGP(info, "Loading GainMap from file {}", refGainMapFile);
      mIDCFactorization.setGainMap(refGainMapFile.data(), "GainMap");
    }

    // disabling CCDB output in order to avoid writing ICCs to IDC CCDB storage!
    if (mProcessClusters) {
      mSendOutCCDB = false;
    }
  }

  void run(o2::framework::ProcessingContext& pc) final
  {
    // store precise timestamp and hbf per TF for look up later only once
    if (mUsePrecisetimeStamp && pc.inputs().isValid("orbitreset")) {
      mTFInfo = pc.inputs().get<dataformats::Pair<long, int>>("orbitreset");
      if (pc.inputs().countValidInputs() == 1) {
        return;
      }
    }

    const auto currTF = processing_helpers::getCurrentTF(pc);
    if ((mTFFirst == -1) && pc.inputs().isValid("firstTF")) {
      mTFFirst = pc.inputs().get<long>("firstTF");
    }

    if (mTFFirst == -1) {
      mTFFirst = currTF;
      LOGP(warning, "firstTF not Found!!! Found valid inputs {}. Setting {} as first TF", pc.inputs().countValidInputs(), mTFFirst);
    }

    // set data taking context only once
    if (mSetDataTakingCont) {
      mDataTakingContext = pc.services().get<DataTakingContext>();
      mSetDataTakingCont = false;
    }

    // set the run number only once
    if (!mRun) {
      mRun = processing_helpers::getRunNumber(pc);
    }

    const long relTF = (mTFFirst == -1) ? 0 : (currTF - mTFFirst) / mNTFsBuffer;

    // set time stamp only once for each aggregation interval
    if (mTimestampStart == 0) {
      setTimeStampCCDB(relTF, pc);
    }

    // loop over input data
    for (auto& ref : InputRecordWalker(pc.inputs(), mFilter)) {
      ++mProcessedCRUs;
      if ((relTF >= mIDCFactorization.getNTimeframes()) || (relTF < 0)) {
        continue;
      }

      auto data = pc.inputs().get<std::vector<float>>(ref);
      if (data.empty()) {
        continue;
      }

      auto const* tpcCRUHeader = o2::framework::DataRefUtils::getHeader<o2::header::DataHeader*>(ref);
      const unsigned int cru = tpcCRUHeader->subSpecification;
      mIDCFactorization.setIDCs(std::move(data), cru, relTF);
    }

    if (mProcessedCRUs == mCRUs.size() * mIDCFactorization.getNTimeframes()) {
      LOGP(info, "ProcessedTFs: {}   currTF: {}  relTF: {}  OrbitResetTime: {} orbits per TF: {}", mProcessedCRUs / mCRUs.size(), currTF, relTF, mTFInfo.first, mTFInfo.second);
      factorizeIDCs();
      sendOutput(pc.outputs());
    }
  }

  void endOfStream(o2::framework::EndOfStreamContext& ec) final
  {
    factorizeIDCs();
    sendOutput(ec.outputs());
    ec.services().get<ControlService>().readyToQuit(QuitRequest::Me);
  }

  static constexpr header::DataDescription getDataDescriptionIDC0() { return header::DataDescription{"IDC0"}; }
  static constexpr header::DataDescription getDataDescriptionIDC1() { return header::DataDescription{"IDC1"}; }
  static constexpr header::DataDescription getDataDescriptionTimeStamp() { return header::DataDescription{"FOURIERTS"}; }
  static constexpr header::DataDescription getDataDescriptionIntervals() { return header::DataDescription{"INTERVALS"}; }
  static constexpr header::DataDescription getDataDescriptionIDCDelta() { return header::DataDescription{"IDCDELTA"}; }
  static constexpr header::DataDescription getDataDescriptionFourier() { return header::DataDescription{"FOURIER"}; }
  static constexpr header::DataDescription getDataDescriptionLane() { return header::DataDescription{"IDCLANE"}; }

  // for CCDB
  static constexpr header::DataDescription getDataDescriptionCCDBGroupingPar() { return header::DataDescription{"TPC_CalibGrParam"}; }
  static constexpr header::DataDescription getDataDescriptionCCDBIDC0() { return header::DataDescription{"TPC_CalibIDC0"}; }
  static constexpr header::DataDescription getDataDescriptionCCDBIDC1() { return header::DataDescription{"TPC_CalibIDC1"}; }
  static constexpr header::DataDescription getDataDescriptionCCDBIDCDelta() { return header::DataDescription{"TPC_IDCDelta"}; }
  static constexpr header::DataDescription getDataDescriptionCCDBIDCPadFlag() { return header::DataDescription{"TPC_CalibFlags"}; }

 private:
  const std::vector<uint32_t> mCRUs{}; ///< CRUs to process in this instance
  unsigned int mProcessedCRUs{};       ///< number of processed CRUs to keep track of when the writing to CCDB etc. will be done
  std::string mMetaFileDir{};
  std::string mCalibFileDir{};
  IDCFactorization mIDCFactorization;               ///< object aggregating the IDCs and performing the factorization of the IDCs
  IDCAverageGroup<IDCAverageGroupTPC> mIDCGrouping; ///< object for averaging and grouping of the IDCs
  const IDCDeltaCompression mCompressionDeltaIDC{}; ///< compression type for IDC Delta
  const bool mUsePrecisetimeStamp{true};            ///< use precise time stamp when writing to CCDB
  const bool mSendOutFFT{false};                    ///<  flag if the output will be send for the FFT
  bool mSendOutCCDB{false};                         ///< sending the outputs for ccdb populator
  long mTFFirst{-1};                                ///< first TF of current aggregation interval
  bool mUpdateGroupingPar{true};                    ///< flag to set if grouping parameters should be updated or not
  const int mLaneId{0};                             ///< the id of the current process within the parallel pipeline
  std::vector<Side> mSides{};                       ///< processed TPC sides
  const int mNTFsBuffer{1};                         ///< number of TFs for which the IDCs will be buffered
  std::unique_ptr<CalDet<PadFlags>> mPadFlagsMap;   ///< status flag for each pad (i.e. if the pad is dead). This map is buffered to check if something changed, when a new map is created
  int mNOrbitsIDC{12};                              ///< Number of orbits over which the IDCs are integrated.
  bool mDumpIDC0{false};                            ///< Dump IDC0 to file
  bool mDumpIDC1{false};                            ///< Dump IDC1 to file
  bool mDumpIDCDelta{false};                        ///< Dump IDCDelta to file
  bool mDumpIDCDeltaCalibData{false};               ///< dump the IDC Delta as a calibration file
  bool mDumpIDCs{false};                            ///< dump IDCs to file
  bool mOffsetCCDB{false};                          ///< flag for setting and offset for CCDB timestamp
  bool mDisableIDCDelta{false};                     ///< disable the processing and storage of IDCDelta
  dataformats::Pair<long, int> mTFInfo{};           ///< orbit reset time for CCDB time stamp writing
  bool mEnableWritingPadStatusMap{false};           ///< do not store the pad status map in the CCDB
  o2::framework::DataTakingContext mDataTakingContext{};
  bool mSetDataTakingCont{true};
  const bool mProcessClusters{false};
  long mTimestampStart{0};                                                                                                                                                                  ///< time stamp of first TF
  uint64_t mRun{0};                                                                                                                                                                         ///< run number
  const std::vector<InputSpec> mFilter = {{"idcagg", ConcreteDataTypeMatcher{gDataOriginTPC, TPCDistributeIDCSpec::getDataDescriptionIDC(mLaneId, mProcessClusters)}, Lifetime::Sporadic}}; ///< filter for looping over input data

  void sendOutput(DataAllocator& output)
  {
    using timer = std::chrono::high_resolution_clock;
    const auto offsetCCDB = mOffsetCCDB ? o2::ccdb::CcdbObjectInfo::HOUR : 0;
    const long timeStampEnd = offsetCCDB + mTimestampStart + mNOrbitsIDC * mIDCFactorization.getNIntegrationIntervals() * o2::constants::lhc::LHCOrbitMUS * 0.001;
    LOGP(info, "Setting time stamp range from {} to {} for writing to CCDB with an offset of {}", mTimestampStart, timeStampEnd, offsetCCDB);

    // sending output to FFT
    if (mSendOutFFT) {
      for (const auto side : mSides) {
        const unsigned int iSide = static_cast<unsigned int>(side);
        LOGP(info, "Sending IDC1 for side {} of size {}", iSide, mIDCFactorization.getIDCOneVec(side).size());
        output.snapshot(Output{gDataOriginTPC, getDataDescriptionIDC1(), header::DataHeader::SubSpecificationType{iSide}}, mIDCFactorization.getIDCOneVec(side));
      }
      output.snapshot(Output{gDataOriginTPC, getDataDescriptionTimeStamp()}, std::vector<long>{mTimestampStart, timeStampEnd});
      output.snapshot(Output{gDataOriginTPC, getDataDescriptionIntervals()}, mIDCFactorization.getIntegrationIntervalsPerTF());
      output.snapshot(Output{gDataOriginTPC, getDataDescriptionLane()}, mLaneId);
    }

    if (mSendOutCCDB) {
      for (int iSide = 0; iSide < mSides.size(); ++iSide) {
        const Side side = mSides[iSide];
        LOGP(info, "Writing IDCs to CCDB for Side {}", static_cast<int>(side));
        const bool sideA = side == Side::A;

        // write struct containing grouping parameters to access grouped IDCs to CCDB
        if (mUpdateGroupingPar) {
          ParameterIDCGroupCCDB object = mIDCGrouping.getIDCGroupHelperSector().getGroupingParameter();
          o2::ccdb::CcdbObjectInfo ccdbInfo(CDBTypeMap.at(sideA ? CDBType::CalIDCGroupingParA : CDBType::CalIDCGroupingParC), std::string{}, std::string{}, std::map<std::string, std::string>{}, mTimestampStart, o2::ccdb::CcdbObjectInfo::INFINITE_TIMESTAMP);
          auto image = o2::ccdb::CcdbApi::createObjectImage(&object, &ccdbInfo);
          LOGP(info, "Sending object {} / {} of size {} bytes, valid for {} : {} ", ccdbInfo.getPath(), ccdbInfo.getFileName(), image->size(), ccdbInfo.getStartValidityTimestamp(), ccdbInfo.getEndValidityTimestamp());
          output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBPayload, getDataDescriptionCCDBGroupingPar(), 0}, *image.get());
          output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBWrapper, getDataDescriptionCCDBGroupingPar(), 0}, ccdbInfo);
          mUpdateGroupingPar = false; // write grouping parameters only once
        }

        auto start = timer::now();
        o2::ccdb::CcdbObjectInfo ccdbInfoIDC0(CDBTypeMap.at(sideA ? CDBType::CalIDC0A : CDBType::CalIDC0C), std::string{}, std::string{}, std::map<std::string, std::string>{}, mTimestampStart, timeStampEnd);
        auto imageIDC0 = o2::ccdb::CcdbApi::createObjectImage(&mIDCFactorization.getIDCZero(side), &ccdbInfoIDC0);
        LOGP(info, "Sending object {} / {} of size {} bytes, valid for {} : {} ", ccdbInfoIDC0.getPath(), ccdbInfoIDC0.getFileName(), imageIDC0->size(), ccdbInfoIDC0.getStartValidityTimestamp(), ccdbInfoIDC0.getEndValidityTimestamp());
        output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBPayload, getDataDescriptionCCDBIDC0(), 0}, *imageIDC0.get());
        output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBWrapper, getDataDescriptionCCDBIDC0(), 0}, ccdbInfoIDC0);
        auto stop = timer::now();
        std::chrono::duration<float> time = stop - start;
        float totalTime = time.count();
        LOGP(info, "IDCZero CCDB time: {}", time.count());

        start = timer::now();
        o2::ccdb::CcdbObjectInfo ccdbInfoIDC1(CDBTypeMap.at(sideA ? CDBType::CalIDC1A : CDBType::CalIDC1C), std::string{}, std::string{}, std::map<std::string, std::string>{}, mTimestampStart, timeStampEnd);
        auto imageIDC1 = o2::ccdb::CcdbApi::createObjectImage(&mIDCFactorization.getIDCOne(side), &ccdbInfoIDC1);
        LOGP(info, "Sending object {} / {} of size {} bytes, valid for {} : {} ", ccdbInfoIDC1.getPath(), ccdbInfoIDC1.getFileName(), imageIDC1->size(), ccdbInfoIDC1.getStartValidityTimestamp(), ccdbInfoIDC1.getEndValidityTimestamp());
        output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBPayload, getDataDescriptionCCDBIDC1(), 0}, *imageIDC1.get());
        output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBWrapper, getDataDescriptionCCDBIDC1(), 0}, ccdbInfoIDC1);
        stop = timer::now();
        time = stop - start;
        LOGP(info, "IDC1 CCDB time: {}", time.count());
        totalTime += time.count();

        auto padStatusMap = mIDCFactorization.getPadStatusMap();
        if (padStatusMap && iSide == 0) {
          start = timer::now();

          // store map in case it is no nullptr
          if (mEnableWritingPadStatusMap) {
            if (!mPadFlagsMap) {
              mPadFlagsMap = std::move(padStatusMap);
              LOGP(info, "Writing pad status map to CCDB.");
              o2::ccdb::CcdbObjectInfo ccdbInfoPadFlags(CDBTypeMap.at(sideA ? CDBType::CalIDCPadStatusMapA : CDBType::CalIDCPadStatusMapC), std::string{}, std::string{}, std::map<std::string, std::string>{}, mTimestampStart, timeStampEnd);
              auto imageFlagMap = o2::ccdb::CcdbApi::createObjectImage(mPadFlagsMap.get(), &ccdbInfoPadFlags);
              LOGP(info, "Sending object {} / {} of size {} bytes, valid for {} : {} ", ccdbInfoPadFlags.getPath(), ccdbInfoPadFlags.getFileName(), imageFlagMap->size(), ccdbInfoPadFlags.getStartValidityTimestamp(), ccdbInfoPadFlags.getEndValidityTimestamp());
              output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBPayload, getDataDescriptionCCDBIDCPadFlag(), 0}, *imageFlagMap.get());
              output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBWrapper, getDataDescriptionCCDBIDCPadFlag(), 0}, ccdbInfoPadFlags);
              LOGP(info, "Pad status map written to CCDB");
            } else {
              // check if map changed. if it changed update the map in the CCDB and store new map in buffer
              if (!(*padStatusMap.get() == *mPadFlagsMap.get())) {
                LOGP(info, "Pad status map changed");
                LOGP(info, "Writing pad status map to CCDB");
                o2::ccdb::CcdbObjectInfo ccdbInfoPadFlags(CDBTypeMap.at(sideA ? CDBType::CalIDCPadStatusMapA : CDBType::CalIDCPadStatusMapC), std::string{}, std::string{}, std::map<std::string, std::string>{}, mTimestampStart, timeStampEnd);
                auto imageFlagMap = o2::ccdb::CcdbApi::createObjectImage(mPadFlagsMap.get(), &ccdbInfoPadFlags);
                LOGP(info, "Sending object {} / {} of size {} bytes, valid for {} : {} ", ccdbInfoPadFlags.getPath(), ccdbInfoPadFlags.getFileName(), imageFlagMap->size(), ccdbInfoPadFlags.getStartValidityTimestamp(), ccdbInfoPadFlags.getEndValidityTimestamp());
                output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBPayload, getDataDescriptionCCDBIDCPadFlag(), 0}, *imageFlagMap.get());
                output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBWrapper, getDataDescriptionCCDBIDCPadFlag(), 0}, ccdbInfoPadFlags);
                LOGP(info, "Pad status map written to CCDB");
              }
              stop = timer::now();
              time = stop - start;
              LOGP(info, "Pad status map CCDB time: {}", time.count());
              totalTime += time.count();
            }
          }
          stop = timer::now();
          time = stop - start;
          LOGP(info, "Pad status map CCDB time: {}", time.count());
          totalTime += time.count();
        }

        if (!mDisableIDCDelta || mDumpIDCDeltaCalibData) {
          start = timer::now();
          for (unsigned int iChunk = 0; iChunk < mIDCFactorization.getNChunks(side); ++iChunk) {
            auto startGrouping = timer::now();
            mIDCGrouping.setIDCs(std::move(mIDCFactorization).getIDCDeltaUncompressed(iChunk, side), side);
            mIDCGrouping.processIDCs(mIDCFactorization.getUsePadStatusMap() ? mPadFlagsMap.get() : nullptr);
            auto stopGrouping = timer::now();
            time = stopGrouping - startGrouping;
            LOGP(info, "Averaging and grouping time: {}", time.count());

            const long timeStampStartDelta = mTimestampStart + mNOrbitsIDC * mIDCFactorization.getNIntegrationIntervalsToChunk(iChunk) * o2::constants::lhc::LHCOrbitMUS * 0.001;
            const long timeStampEndDelta = offsetCCDB + timeStampStartDelta + mNOrbitsIDC * mIDCFactorization.getNIntegrationIntervalsInChunk(iChunk) * o2::constants::lhc::LHCOrbitMUS * 0.001;
            o2::ccdb::CcdbObjectInfo ccdbInfoIDCDelta(CDBTypeMap.at(sideA ? CDBType::CalIDCDeltaA : CDBType::CalIDCDeltaC), std::string{}, std::string{}, std::map<std::string, std::string>{}, timeStampStartDelta, timeStampEndDelta);

            if (mDumpIDCDelta) {
              mIDCGrouping.dumpToFile(fmt::format("{}DeltaAveraged_chunk{:02}_{:02}_side{}.root", getCurrentType(), iChunk, timeStampStartDelta, side).data());
            }

            auto startCCDBIDCDelta = timer::now();
            std::unique_ptr<std::vector<char>> imageIDCDelta;
            switch (mCompressionDeltaIDC) {
              case IDCDeltaCompression::MEDIUM:
              default: {
                using compType = unsigned short;
                IDCDelta<compType> idcDelta = IDCDeltaCompressionHelper<compType>::getCompressedIDCs(mIDCGrouping.getIDCGroupData());
                imageIDCDelta = o2::ccdb::CcdbApi::createObjectImage(&idcDelta, &ccdbInfoIDCDelta);
                break;
              }
              case IDCDeltaCompression::HIGH: {
                using compType = unsigned char;
                IDCDelta<compType> idcDelta = IDCDeltaCompressionHelper<compType>::getCompressedIDCs(mIDCGrouping.getIDCGroupData());
                imageIDCDelta = o2::ccdb::CcdbApi::createObjectImage(&idcDelta, &ccdbInfoIDCDelta);
                break;
              }
              case IDCDeltaCompression::NO:
                IDCDelta<float> idcDelta = std::move(mIDCGrouping).getIDCGroupData();
                imageIDCDelta = o2::ccdb::CcdbApi::createObjectImage(&idcDelta, &ccdbInfoIDCDelta);
                break;
            }

            if (!mDisableIDCDelta) {
              LOGP(info, "Sending object {} / {} of size {} bytes, valid for {} : {} ", ccdbInfoIDCDelta.getPath(), ccdbInfoIDCDelta.getFileName(), imageIDCDelta->size(), ccdbInfoIDCDelta.getStartValidityTimestamp(), ccdbInfoIDCDelta.getEndValidityTimestamp());
              output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBPayload, getDataDescriptionCCDBIDCDelta(), iChunk}, *imageIDCDelta.get());
              output.snapshot(Output{o2::calibration::Utils::gDataOriginCDBWrapper, getDataDescriptionCCDBIDCDelta(), iChunk}, ccdbInfoIDCDelta);
            }

            if (mDumpIDCDeltaCalibData && mCalibFileDir != "/dev/null") {
              const std::string sideStr = sideA ? "A" : "C";
              std::string calibFName = fmt::format("IDCDelta_side{}_cal_data_{}.root", sideStr, ccdbInfoIDCDelta.getStartValidityTimestamp());
              try {
                std::ofstream calFile(fmt::format("{}{}", mCalibFileDir, calibFName), std::ios::out | std::ios::binary);
                calFile.write(imageIDCDelta->data(), imageIDCDelta->size());
                calFile.close();
              } catch (std::exception const& e) {
                LOG(error) << "Failed to store IDC calibration data file " << calibFName << ", reason: " << e.what();
              }

              if (mMetaFileDir != "/dev/null") {
                o2::dataformats::FileMetaData calMetaData;
                calMetaData.fillFileData(calibFName);
                calMetaData.setDataTakingContext(mDataTakingContext);
                calMetaData.type = "calib";
                calMetaData.priority = "low";
                auto metaFileNameTmp = fmt::format("{}{}.tmp", mMetaFileDir, calibFName);
                auto metaFileName = fmt::format("{}{}.done", mMetaFileDir, calibFName);
                try {
                  std::ofstream metaFileOut(metaFileNameTmp);
                  metaFileOut << calMetaData;
                  metaFileOut.close();
                  std::filesystem::rename(metaFileNameTmp, metaFileName);
                } catch (std::exception const& e) {
                  LOG(error) << "Failed to store CTF meta data file " << metaFileName << ", reason: " << e.what();
                }
              }
            }

            auto stopCCDBIDCDelta = timer::now();
            time = stopCCDBIDCDelta - startCCDBIDCDelta;
            LOGP(info, "Compression and CCDB object creation time: {}", time.count());
          }

          stop = timer::now();
          time = stop - start;
          LOGP(info, "IDCDelta CCDB time: {}", time.count());
        }
        totalTime += time.count();
        LOGP(info, "CCDB object creation done. Total time: {}", totalTime);
      }
    }

    // reseting aggregated IDCs. This is done for safety, but if all data is received in the next aggregation interval it isnt necessary... remove it?
    LOGP(info, "Everything done! Clearing memory...");
    mIDCFactorization.reset();
    mTimestampStart = 0;
    mTFFirst = -1;
    mProcessedCRUs = 0; // reset processed TFs for next aggregation interval
    LOGP(info, "Everything cleared. Waiting for new data to arrive.");
  }

  void setTimeStampCCDB(const long relTF, o2::framework::ProcessingContext& pc)
  {
    // return if the tf info is not set
    if (mUsePrecisetimeStamp && !mTFInfo.second) {
      return;
    }
    const auto& tinfo = pc.services().get<o2::framework::TimingInfo>();
    const auto nOrbitsOffset = (relTF * mNTFsBuffer + (mNTFsBuffer - 1)) * mTFInfo.second; // offset to first orbit of IDCs of current orbit
    mTimestampStart = mUsePrecisetimeStamp ? (mTFInfo.first + (tinfo.firstTForbit - nOrbitsOffset) * o2::constants::lhc::LHCOrbitMUS * 0.001) : tinfo.creation;
    LOGP(info, "setting time stamp reset reference to: {}, at tfCounter: {}, firstTForbit: {}, NHBFPerTF: {}, relTF: {}, nOrbitsOffset: {}", mTFInfo.first, tinfo.tfCounter, tinfo.firstTForbit, mTFInfo.second, relTF, nOrbitsOffset);
  }

  void factorizeIDCs()
  {
    if (mDumpIDCs) {
      LOGP(info, "dumping aggregated and factorized IDCs to file for mTFFirst {}", mTFFirst);
      mIDCFactorization.setTimeStamp(mTimestampStart);
      mIDCFactorization.setRun(mRun);
      mIDCFactorization.dumpLargeObjectToFile(fmt::format("{}Factorized_TF_{:02}_TS_{}.root", getCurrentType(), mTFFirst, mTimestampStart).data());
    }

    mIDCFactorization.factorizeIDCs(true, !mDisableIDCDelta); // calculate DeltaIDC, 0D-IDC, 1D-IDC

    if (mDumpIDC0) {
      LOGP(info, "dumping IDC Zero to file");
      for (auto side : mIDCFactorization.getSides()) {
        const std::string outFileName = (side == Side::A) ? fmt::format("{}Zero_A_{:02}.root", getCurrentType(), mTFFirst) : fmt::format("{}Zero_C_{:02}.root", getCurrentType(), mTFFirst);
        mIDCFactorization.dumpIDCZeroToFile(side, outFileName.data());
      }
    }

    if (mDumpIDC1) {
      LOGP(info, "dumping IDC1 to file");
      for (auto side : mIDCFactorization.getSides()) {
        const std::string outFileName = (side == Side::A) ? fmt::format("{}One_A_{:02}.root", getCurrentType(), mTFFirst) : fmt::format("{}One_C_{:02}.root", getCurrentType(), mTFFirst);
        mIDCFactorization.dumpIDCOneToFile(side, outFileName.data());
      }
    }
  }

  std::string getCurrentType() const { return mProcessClusters ? "ICC" : "IDC"; }
};

DataProcessorSpec getTPCFactorizeIDCSpec(const int lane, const std::vector<uint32_t>& crus, const unsigned int timeframes, const unsigned int timeframesDeltaIDC, const IDCDeltaCompression compression, const bool usePrecisetimeStamp, const bool sendOutputFFT, const bool sendCCDB, const int nTFsBuffer = 1, const bool processClusters = false)
{
  const auto sides = o2::tpc::IDCFactorization::getSides(crus);

  std::vector<OutputSpec> outputSpecs;
  if (sendCCDB) {
    outputSpecs.emplace_back(ConcreteDataTypeMatcher{o2::calibration::Utils::gDataOriginCDBPayload, TPCFactorizeIDCSpec::getDataDescriptionCCDBGroupingPar()}, Lifetime::Sporadic);
    outputSpecs.emplace_back(ConcreteDataTypeMatcher{o2::calibration::Utils::gDataOriginCDBWrapper, TPCFactorizeIDCSpec::getDataDescriptionCCDBGroupingPar()}, Lifetime::Sporadic);
    outputSpecs.emplace_back(ConcreteDataTypeMatcher{o2::calibration::Utils::gDataOriginCDBPayload, TPCFactorizeIDCSpec::getDataDescriptionCCDBIDC0()}, Lifetime::Sporadic);
    outputSpecs.emplace_back(ConcreteDataTypeMatcher{o2::calibration::Utils::gDataOriginCDBWrapper, TPCFactorizeIDCSpec::getDataDescriptionCCDBIDC0()}, Lifetime::Sporadic);
    outputSpecs.emplace_back(ConcreteDataTypeMatcher{o2::calibration::Utils::gDataOriginCDBPayload, TPCFactorizeIDCSpec::getDataDescriptionCCDBIDC1()}, Lifetime::Sporadic);
    outputSpecs.emplace_back(ConcreteDataTypeMatcher{o2::calibration::Utils::gDataOriginCDBWrapper, TPCFactorizeIDCSpec::getDataDescriptionCCDBIDC1()}, Lifetime::Sporadic);
    outputSpecs.emplace_back(ConcreteDataTypeMatcher{o2::calibration::Utils::gDataOriginCDBPayload, TPCFactorizeIDCSpec::getDataDescriptionCCDBIDCDelta()}, Lifetime::Sporadic);
    outputSpecs.emplace_back(ConcreteDataTypeMatcher{o2::calibration::Utils::gDataOriginCDBWrapper, TPCFactorizeIDCSpec::getDataDescriptionCCDBIDCDelta()}, Lifetime::Sporadic);
    outputSpecs.emplace_back(ConcreteDataTypeMatcher{o2::calibration::Utils::gDataOriginCDBPayload, TPCFactorizeIDCSpec::getDataDescriptionCCDBIDCPadFlag()}, Lifetime::Sporadic);
    outputSpecs.emplace_back(ConcreteDataTypeMatcher{o2::calibration::Utils::gDataOriginCDBWrapper, TPCFactorizeIDCSpec::getDataDescriptionCCDBIDCPadFlag()}, Lifetime::Sporadic);
  }

  if (sendOutputFFT) {
    for (auto side : sides) {
      outputSpecs.emplace_back(ConcreteDataMatcher{gDataOriginTPC, TPCFactorizeIDCSpec::getDataDescriptionIDC1(), header::DataHeader::SubSpecificationType{side}}, Lifetime::Sporadic);
    }
    outputSpecs.emplace_back(ConcreteDataMatcher{gDataOriginTPC, TPCFactorizeIDCSpec::getDataDescriptionTimeStamp(), header::DataHeader::SubSpecificationType{0}}, Lifetime::Sporadic);
    outputSpecs.emplace_back(ConcreteDataMatcher{gDataOriginTPC, TPCFactorizeIDCSpec::getDataDescriptionIntervals(), header::DataHeader::SubSpecificationType{0}}, Lifetime::Sporadic);
    outputSpecs.emplace_back(ConcreteDataMatcher{gDataOriginTPC, TPCFactorizeIDCSpec::getDataDescriptionLane(), header::DataHeader::SubSpecificationType{0}}, Lifetime::Sporadic);
  }

  std::vector<InputSpec> inputSpecs;
  inputSpecs.emplace_back(InputSpec{"idcagg", ConcreteDataTypeMatcher{gDataOriginTPC, TPCDistributeIDCSpec::getDataDescriptionIDC(lane, processClusters)}, Lifetime::Sporadic});
  inputSpecs.emplace_back(InputSpec{"firstTF", gDataOriginTPC, TPCDistributeIDCSpec::getDataDescriptionIDCFirstTF(), header::DataHeader::SubSpecificationType{static_cast<unsigned int>(lane)}, Lifetime::Sporadic});
  if (usePrecisetimeStamp) {
    inputSpecs.emplace_back(InputSpec{"orbitreset", gDataOriginTPC, TPCDistributeIDCSpec::getDataDescriptionIDCOrbitReset(), header::DataHeader::SubSpecificationType{static_cast<unsigned int>(lane)}, Lifetime::Sporadic});
  }

  const auto& paramIDCGroup = ParameterIDCGroup::Instance();
  std::array<unsigned char, Mapper::NREGIONS> groupPads{};
  std::array<unsigned char, Mapper::NREGIONS> groupRows{};
  std::array<unsigned char, Mapper::NREGIONS> groupLastRowsThreshold{};
  std::array<unsigned char, Mapper::NREGIONS> groupLastPadsThreshold{};
  std::copy(std::begin(paramIDCGroup.groupPads), std::end(paramIDCGroup.groupPads), std::begin(groupPads));
  std::copy(std::begin(paramIDCGroup.groupRows), std::end(paramIDCGroup.groupRows), std::begin(groupRows));
  std::copy(std::begin(paramIDCGroup.groupLastRowsThreshold), std::end(paramIDCGroup.groupLastRowsThreshold), std::begin(groupLastRowsThreshold));
  std::copy(std::begin(paramIDCGroup.groupLastPadsThreshold), std::end(paramIDCGroup.groupLastPadsThreshold), std::begin(groupLastPadsThreshold));
  const unsigned int groupPadsSectorEdges = paramIDCGroup.groupPadsSectorEdges;

  const std::string type = processClusters ? "icc" : "idc";
  DataProcessorSpec spec{
    fmt::format("tpc-factorize-{}-{:02}", type, lane).data(),
    inputSpecs,
    outputSpecs,
    AlgorithmSpec{adaptFromTask<TPCFactorizeIDCSpec>(crus, timeframes, timeframesDeltaIDC, groupPads, groupRows, groupLastRowsThreshold, groupLastPadsThreshold, groupPadsSectorEdges, compression, usePrecisetimeStamp, sendOutputFFT, sendCCDB, lane, sides, nTFsBuffer, processClusters)},
    Options{{"gainMapFile", VariantType::String, "", {"file to reference gain map, which will be used for correcting the cluster charge"}},
            {"enablePadStatusMap", VariantType::Bool, false, {"Enabling the usage of the pad-by-pad status map during factorization."}},
            {"enableWritingPadStatusMap", VariantType::Bool, false, {"Write the pad status map to CCDB."}},
            {"orbits-IDCs", VariantType::Int, 12, {"Number of orbits over which the IDCs are integrated."}},
            {"dump-IDCs", VariantType::Bool, false, {"Dump IDCs to file"}},
            {"dump-IDC0", VariantType::Bool, false, {"Dump IDC0 to file"}},
            {"dump-IDC1", VariantType::Bool, false, {"Dump IDC1 to file"}},
            {"disable-IDCDelta", VariantType::Bool, false, {"Disable processing of IDCDelta and storage in the CCDB"}},
            {"dump-IDCDelta", VariantType::Bool, false, {"Dump IDCDelta to file"}},
            {"dump-IDCDelta-calib-data", VariantType::Bool, false, {"Dump IDCDelta as calibration data to file"}},
            {"add-offset-for-CCDB-timestamp", VariantType::Bool, false, {"Add an offset of 1 hour for the validity range of the CCDB objects"}},
            {"output-dir", VariantType::String, "none", {"calibration files output directory, must exist"}},
            {"meta-output-dir", VariantType::String, "/dev/null", {"calibration metadata output directory, must exist (if not /dev/null)"}},
            {"update-not-grouping-parameter", VariantType::Bool, false, {"Do NOT Update/Writing grouping parameters to CCDB."}}}}; // end DataProcessorSpec
  spec.rank = lane;
  return spec;
}

} // namespace o2::tpc

#endif
