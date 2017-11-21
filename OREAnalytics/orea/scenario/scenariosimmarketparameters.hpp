/*
 Copyright (C) 2016 Quaternion Risk Management Ltd
 All rights reserved.

 This file is part of ORE, a free-software/open-source library
 for transparent pricing and risk analysis - http://opensourcerisk.org

 ORE is free software: you can redistribute it and/or modify it
 under the terms of the Modified BSD License.  You should have received a
 copy of the license along with this program.
 The license is also available online at <http://opensourcerisk.org>

 This program is distributed on the basis that it will form a useful
 contribution to risk analytics and model standardisation, but WITHOUT
 ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 FITNESS FOR A PARTICULAR PURPOSE. See the license for more details.
*/

/*! \file scenario/scenariosimmarketparameters.hpp
    \brief A class to hold Scenario parameters for scenarioSimMarket
    \ingroup scenario
*/

#pragma once

#include <ored/utilities/parsers.hpp>
#include <ored/utilities/xmlutils.hpp>
#include <qle/termstructures/dynamicstype.hpp>

using QuantLib::Date;
using QuantLib::Period;
using QuantLib::Rate;
using std::vector;
using std::string;
using std::pair;
using ore::data::XMLSerializable;
using ore::data::XMLNode;
using ore::data::XMLUtils;

namespace ore {
namespace analytics {

//! ScenarioSimMarket description
/*! \ingroup scenario
 */
class ScenarioSimMarketParameters : public XMLSerializable {
public:
    //! Default constructor
    ScenarioSimMarketParameters()
        : extrapolate_(false), swapVolSimulate_(false), swapVolIsCube_(false), swapVolSimulateATMOnly_(true), swapVolStrikeSpreads_({0}), 
          capFloorVolSimulate_(false),  survivalProbabilitySimulate_(false), recoveryRateSimulate_(false), cdsVolSimulate_(false), 
          equityForecastCurveSimulate_(true), dividendYieldSimulate_(false),
          fxVolSimulate_(false), fxVolIsSurface_(false), equityVolSimulate_(false), equityIsSurface_(false), 
          equityVolSimulateATMOnly_(true), equityMoneyness_({1}), baseCorrelationSimulate_(false) {
        // set default tenors
        capFloorVolExpiries_[""];
        defaultTenors_[""];
        equityDividendTenors_[""];
        equityForecastTenors_[""];
        zeroInflationTenors_[""];
        yoyInflationTenors_[""];
    }

    //! \name Inspectors
    //@{
    const string& baseCcy() const { return baseCcy_; }
    const vector<string>& ccys() const { return ccys_; }
    const string& yieldCurveDayCounter() const { return yieldCurveDayCounter_; }
    const vector<string>& yieldCurveNames() const { return yieldCurveNames_; }
    const vector<string>& yieldCurveCurrencies() const { return yieldCurveCurrencies_; }
    const vector<Period>& yieldCurveTenors(const string& key) const;
    bool hasYieldCurveTenors(const string& key) const { return yieldCurveTenors_.count(key) > 0; }
    const vector<string>& indices() const { return indices_; }
    const map<string, string>& swapIndices() const { return swapIndices_; }
    const string& interpolation() const { return interpolation_; }
    bool extrapolate() const { return extrapolate_; }

    const vector<string>& fxCcyPairs() const { return fxCcyPairs_; }

    bool simulateSwapVols() const { return swapVolSimulate_; }
    bool swapVolIsCube() const { return swapVolIsCube_; }
    bool simulateSwapVolATMOnly() const { return swapVolSimulateATMOnly_; }
    const vector<Period>& swapVolTerms() const { return swapVolTerms_; }
    const vector<Period>& swapVolExpiries() const { return swapVolExpiries_; }
    const vector<string>& swapVolCcys() const { return swapVolCcys_; }
    const string& swapVolDcs() const { return swapVolDcs_; }
    const string& swapVolDecayMode() const { return swapVolDecayMode_; }
    const vector<Real>& swapVolStrikeSpreads() const { return swapVolStrikeSpreads_; }

    bool simulateCapFloorVols() const { return capFloorVolSimulate_; }
    const vector<string>& capFloorVolCcys() const { return capFloorVolCcys_; }
    const string& capFloorVolDcs() const { return capFloorVolDcs_; }
    const vector<Period>& capFloorVolExpiries(const string& key) const;
    bool hasCapFloorVolExpiries(const string& key) const { return capFloorVolExpiries_.count(key) > 0; }
    const vector<Real>& capFloorVolStrikes() const { return capFloorVolStrikes_; }
    const string& capFloorVolDecayMode() const { return capFloorVolDecayMode_; }

    bool simulateSurvivalProbabilities() const { return survivalProbabilitySimulate_; }
    bool simulateRecoveryRates() const { return recoveryRateSimulate_; }
    const vector<string>& defaultNames() const { return defaultNames_; }
    const string& defaultDcs() const { return defaultDcs_; }
    const string& defaultCal() const { return defaultCal_; }
    const vector<Period>& defaultTenors(const string& key) const;
    bool hasDefaultTenors(const string& key) const { return defaultTenors_.count(key) > 0; }

    bool simulateCdsVols() const { return cdsVolSimulate_; }
    const vector<Period>& cdsVolExpiries() const { return cdsVolExpiries_; }
    const vector<string>& cdsVolNames() const { return cdsVolNames_; }
    const string& cdsVolDecayMode() const { return cdsVolDecayMode_; }

    const vector<string>& equityNames() const { return equityNames_; }
    const vector<Period>& equityDividendTenors(const string& key) const;
    bool hasEquityDividendTenors(const string& key) const { return equityDividendTenors_.count(key) > 0; }
    const vector<Period>& equityForecastTenors(const string& key) const;
    bool hasEquityForecastTenors(const string& key) const { return equityForecastTenors_.count(key) > 0; }

    bool simulateFXVols() const { return fxVolSimulate_; }
    bool fxVolIsSurface() const { return fxVolIsSurface_; }
    const vector<Period>& fxVolExpiries() const { return fxVolExpiries_; }
    const string& fxVolDecayMode() const { return fxVolDecayMode_; }
    const vector<string>& fxVolCcyPairs() const { return fxVolCcyPairs_; }
    const vector<Real>& fxVolMoneyness() const { return fxMoneyness_; }

    bool simulateEquityVols() const { return equityVolSimulate_; }
    bool equityVolIsSurface() const { return equityIsSurface_; }
    bool simulateEquityVolATMOnly() const { return equityVolSimulateATMOnly_; }
    const vector<Period>& equityVolExpiries() const { return equityVolExpiries_; }
    const string& equityVolDecayMode() const { return equityVolDecayMode_; }
    const vector<string>& equityVolNames() const { return equityVolNames_; }
    const vector<Real>& equityVolMoneyness() const { return equityMoneyness_; }

    const vector<string>& additionalScenarioDataIndices() const { return additionalScenarioDataIndices_; }
    const vector<string>& additionalScenarioDataCcys() const { return additionalScenarioDataCcys_; }

    const vector<string>& securities() const { return securities_; }

    bool simulateBaseCorrelations() const { return baseCorrelationSimulate_; }
    const vector<Period>& baseCorrelationTerms() const { return baseCorrelationTerms_; }
    const vector<Real>& baseCorrelationDetachmentPoints() const { return baseCorrelationDetachmentPoints_; }
    const vector<string>& baseCorrelationNames() const { return baseCorrelationNames_; }
    const string& baseCorrelationDcs() const { return baseCorrelationDcs_; }

    const vector<string>& cpiIndices() const { return cpiIndices_; }
    const vector<string>& zeroInflationIndices() const { return zeroInflationIndices_; }
    const string& zeroInflationDcs() const { return zeroInflationDcs_; }
    const vector<Period>& zeroInflationTenors(const string& key) const;
    bool hasZeroInflationTenors(const string& key) const { return zeroInflationTenors_.count(key) > 0; }
    const string& yoyInflationDcs() const { return yoyInflationDcs_; }
    const vector<string>& yoyInflationIndices() const { return yoyInflationIndices_; }
    const vector<Period>& yoyInflationTenors(const string& key) const;
    bool hasYoyInflationTenors(const string& key) const { return yoyInflationTenors_.count(key) > 0; }

    bool simulateEquityForecastCurve() const { return equityForecastCurveSimulate_; }
    bool simulateDividendYield() const { return dividendYieldSimulate_; }

    //@}

    //! \name Setters
    //@{
    string& baseCcy() { return baseCcy_; }
    vector<string>& ccys() { return ccys_; }
    string& yieldCurveDayCounter() { return yieldCurveDayCounter_; }
    vector<string>& yieldCurveNames() { return yieldCurveNames_; }
    vector<string>& yieldCurveCurrencies() { return yieldCurveCurrencies_; }
    void setYieldCurveTenors(const string& key, const vector<Period>& p);
    vector<string>& indices() { return indices_; }
    map<string, string>& swapIndices() { return swapIndices_; }
    string& interpolation() { return interpolation_; }
    bool& extrapolate() { return extrapolate_; }

    vector<string>& fxCcyPairs() { return fxCcyPairs_; }

    bool& simulateSwapVols() { return swapVolSimulate_; }
    bool& swapVolIsCube() { return swapVolIsCube_; }
    bool& simulateSwapVolATMOnly() { return swapVolSimulateATMOnly_; }
    vector<Period>& swapVolTerms() { return swapVolTerms_; }
    vector<string>& swapVolCcys() { return swapVolCcys_; }
    string& swapVolDcs() { return swapVolDcs_; }
    vector<Period>& swapVolExpiries() { return swapVolExpiries_; }
    vector<Real>& swapVolStrikeSpreads() { return swapVolStrikeSpreads_; }
    string& swapVolDecayMode() { return swapVolDecayMode_; }

    bool& simulateCapFloorVols() { return capFloorVolSimulate_; }
    vector<string>& capFloorVolCcys() { return capFloorVolCcys_; }
    string& capFloorVolDcs() { return capFloorVolDcs_; }
    void setCapFloorVolExpiries(const string& key, const vector<Period>& p);
    vector<Real>& capFloorVolStrikes() { return capFloorVolStrikes_; }
    string& capFloorVolDecayMode() { return capFloorVolDecayMode_; }

    bool& simulateSurvivalProbabilities() { return survivalProbabilitySimulate_; }
    bool& simulateRecoveryRates() { return recoveryRateSimulate_; }
    vector<string>& defaultNames() { return defaultNames_; }
    string& defaultDcs() { return defaultDcs_; }
    string& defaultCal() { return defaultCal_; }
    void setDefaultTenors(const string& key, const vector<Period>& p);

    bool& simulateCdsVols() { return cdsVolSimulate_; }
    vector<Period>& cdsVolExpiries() { return cdsVolExpiries_; }
    vector<string>& cdsVolNames() { return cdsVolNames_; }
    string& cdsVolDecayMode() { return cdsVolDecayMode_; }

    vector<string>& equityNames() { return equityNames_; }
    void setEquityDividendTenors(const string& key, const vector<Period>& p);
    void setEquityForecastTenors(const string& key, const vector<Period>& p);
    
    bool& simulateFXVols() { return fxVolSimulate_; }
    bool& fxVolIsSurface() { return fxVolIsSurface_; }
    vector<Period>& fxVolExpiries() { return fxVolExpiries_; }
    string& fxVolDecayMode() { return fxVolDecayMode_; }
    vector<string>& fxVolCcyPairs() { return fxVolCcyPairs_; }
    vector<Real>& fxVolMoneyness() { return fxMoneyness_; }

    bool& simulateEquityVols() { return equityVolSimulate_; }
    bool& equityVolIsSurface() { return equityIsSurface_; }
    bool& simulateEquityVolATMOnly() { return equityVolSimulateATMOnly_; }
    vector<Period>& equityVolExpiries() { return equityVolExpiries_; }
    string& equityVolDecayMode() { return equityVolDecayMode_; }
    vector<string>& equityVolNames() { return equityVolNames_; }
    vector<Real>& equityVolMoneyness() { return equityMoneyness_; }

    vector<string>& additionalScenarioDataIndices() { return additionalScenarioDataIndices_; }
    vector<string>& additionalScenarioDataCcys() { return additionalScenarioDataCcys_; }

    vector<string>& securities() { return securities_; }

    bool& simulateBaseCorrelations() { return baseCorrelationSimulate_; }
    vector<Period>& baseCorrelationTerms() { return baseCorrelationTerms_; }
    vector<Real>& baseCorrelationDetachmentPoints() { return baseCorrelationDetachmentPoints_; }
    vector<string>& baseCorrelationNames() { return baseCorrelationNames_; }
    string& baseCorrelationDcs() { return baseCorrelationDcs_; }

    vector<string>& cpiIndices() { return cpiIndices_; }
    vector<string>& zeroInflationIndices() { return zeroInflationIndices_; }
    string& zeroInflationDcs() { return zeroInflationDcs_; }
    void setZeroInflationTenors(const string& key, const vector<Period>& p);
    vector<string>& yoyInflationIndices() { return yoyInflationIndices_; }
    string& yoyInflationDcs() { return yoyInflationDcs_; }
    void setYoyInflationTenors(const string& key, const vector<Period>& p);

    bool& simulateEquityForecastCurve() { return equityForecastCurveSimulate_; }
    bool& simulateDividendYield() { return dividendYieldSimulate_; }


    //@}

    //! \name Serialisation
    //@{
    virtual void fromXML(XMLNode* node);
    virtual XMLNode* toXML(ore::data::XMLDocument& doc);
    //@}

    //! \name Equality Operators
    //@{
    bool operator==(const ScenarioSimMarketParameters& rhs);
    bool operator!=(const ScenarioSimMarketParameters& rhs);
    //@}

private:
    string baseCcy_;
    string yieldCurveDayCounter_;
    vector<string> ccys_; // may or may not include baseCcy;
    vector<string> yieldCurveNames_;
    vector<string> yieldCurveCurrencies_;
    map<string, vector<Period>> yieldCurveTenors_;
    vector<string> indices_;
    map<string, string> swapIndices_;
    string interpolation_;
    bool extrapolate_;

    vector<string> fxCcyPairs_;

    bool swapVolSimulate_;
    bool swapVolIsCube_;
    bool swapVolSimulateATMOnly_;
    vector<Period> swapVolTerms_;
    vector<string> swapVolCcys_;
    string swapVolDcs_;
    vector<Period> swapVolExpiries_;
    vector<Real> swapVolStrikeSpreads_;
    string swapVolDecayMode_;

    bool capFloorVolSimulate_;
    vector<string> capFloorVolCcys_;
    string capFloorVolDcs_;
    map<string, vector<Period>> capFloorVolExpiries_;
    vector<Real> capFloorVolStrikes_;
    string capFloorVolDecayMode_;

    bool survivalProbabilitySimulate_;
    bool recoveryRateSimulate_;
    vector<string> defaultNames_;
    string defaultDcs_;
    string defaultCal_;
    map<string, vector<Period>> defaultTenors_;

    bool cdsVolSimulate_;
    vector<string> cdsVolNames_;
    vector<Period> cdsVolExpiries_;
    string cdsVolDecayMode_;

    vector<string> equityNames_;
    bool equityForecastCurveSimulate_;
    bool dividendYieldSimulate_;
    map<string, vector<Period>> equityDividendTenors_;
    map<string, vector<Period>> equityForecastTenors_;

    bool fxVolSimulate_;
    bool fxVolIsSurface_;
    vector<Period> fxVolExpiries_;
    string fxVolDecayMode_;
    vector<string> fxVolCcyPairs_;
    vector<Real> fxMoneyness_;

    bool equityVolSimulate_;
    bool equityIsSurface_;
    bool equityVolSimulateATMOnly_;
    vector<Period> equityVolExpiries_;
    string equityVolDecayMode_;
    vector<string> equityVolNames_;
    vector<Real> equityMoneyness_;

    vector<string> additionalScenarioDataIndices_;
    vector<string> additionalScenarioDataCcys_;

    vector<string> securities_;

    bool baseCorrelationSimulate_;
    vector<string> baseCorrelationNames_;
    string baseCorrelationDcs_;
    vector<Period> baseCorrelationTerms_;
    vector<Real> baseCorrelationDetachmentPoints_;
    
    vector<string> cpiIndices_;
    vector<string> zeroInflationIndices_;
    string zeroInflationDcs_;
    map<string, vector<Period>> zeroInflationTenors_;
    vector<string> yoyInflationIndices_;
    string yoyInflationDcs_;
    map<string, vector<Period>> yoyInflationTenors_;

};
} // namespace analytics
} // namespace ore
