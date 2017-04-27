/*
 Copyright (C) 2016, 2017 Quaternion Risk Management Ltd
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

#include "crossassetmodel.hpp"

#include <qle/methods/multipathgeneratorbase.hpp>
#include <qle/models/all.hpp>
#include <qle/pricingengines/all.hpp>
#include <qle/processes/all.hpp>

#include <ql/currencies/america.hpp>
#include <ql/currencies/europe.hpp>
#include <ql/indexes/ibor/euribor.hpp>
#include <ql/indexes/ibor/gbplibor.hpp>
#include <ql/indexes/ibor/usdlibor.hpp>
#include <ql/indexes/inflation/euhicp.hpp>
#include <ql/instruments/cpicapfloor.hpp>
#include <ql/instruments/vanillaoption.hpp>
#include <ql/math/optimization/levenbergmarquardt.hpp>
#include <ql/math/randomnumbers/rngtraits.hpp>
#include <ql/math/statistics/incrementalstatistics.hpp>
#include <ql/methods/montecarlo/multipathgenerator.hpp>
#include <ql/methods/montecarlo/pathgenerator.hpp>
#include <ql/models/shortrate/calibrationhelpers/swaptionhelper.hpp>
#include <ql/models/shortrate/onefactormodels/gsr.hpp>
#include <ql/pricingengines/swaption/gaussian1dswaptionengine.hpp>
#include <ql/quotes/simplequote.hpp>
#include <ql/termstructures/credit/flathazardrate.hpp>
#include <ql/termstructures/inflation/interpolatedzeroinflationcurve.hpp>
#include <ql/termstructures/inflationtermstructure.hpp>
#include <ql/termstructures/yield/flatforward.hpp>
#include <ql/time/calendars/target.hpp>
#include <ql/time/daycounters/actual360.hpp>
#include <ql/time/daycounters/thirty360.hpp>

#include <test-suite/utilities.hpp>

#include <boost/accumulators/accumulators.hpp>
#include <boost/accumulators/statistics/covariance.hpp>
#include <boost/accumulators/statistics/error_of_mean.hpp>
#include <boost/accumulators/statistics/mean.hpp>
#include <boost/accumulators/statistics/stats.hpp>
#include <boost/accumulators/statistics/variates/covariate.hpp>
#include <boost/make_shared.hpp>

using namespace QuantLib;
using namespace QuantExt;
using namespace CrossAssetModelTypes;

using boost::unit_test_framework::test_suite;
using namespace boost::accumulators;

namespace {

struct BermudanTestData {
    BermudanTestData()
        : evalDate(12, January, 2015), yts(boost::make_shared<FlatForward>(evalDate, 0.02, Actual365Fixed())),
          euribor6m(boost::make_shared<Euribor>(6 * Months, yts)), effectiveDate(TARGET().advance(evalDate, 2 * Days)),
          startDate(TARGET().advance(effectiveDate, 1 * Years)), maturityDate(TARGET().advance(startDate, 9 * Years)),
          fixedSchedule(startDate, maturityDate, 1 * Years, TARGET(), ModifiedFollowing, ModifiedFollowing,
                        DateGeneration::Forward, false),
          floatingSchedule(startDate, maturityDate, 6 * Months, TARGET(), ModifiedFollowing, ModifiedFollowing,
                           DateGeneration::Forward, false),
          underlying(
              boost::make_shared<VanillaSwap>(VanillaSwap(VanillaSwap::Payer, 1.0, fixedSchedule, 0.02, Thirty360(),
                                                          floatingSchedule, euribor6m, 0.0, Actual360()))),
          reversion(0.03) {
        Settings::instance().evaluationDate() = evalDate;
        for (Size i = 0; i < 9; ++i) {
            exerciseDates.push_back(TARGET().advance(fixedSchedule[i], -2 * Days));
        }
        exercise = boost::make_shared<BermudanExercise>(exerciseDates, false);

        swaption = boost::make_shared<Swaption>(underlying, exercise);
        stepDates = std::vector<Date>(exerciseDates.begin(), exerciseDates.end() - 1);
        sigmas = std::vector<Real>(stepDates.size() + 1);
        for (Size i = 0; i < sigmas.size(); ++i) {
            sigmas[i] = 0.0050 + (0.0080 - 0.0050) * std::exp(-0.2 * static_cast<double>(i));
        }
        stepTimes_a = Array(stepDates.size());
        for (Size i = 0; i < stepDates.size(); ++i) {
            stepTimes_a[i] = yts->timeFromReference(stepDates[i]);
        }
        sigmas_a = Array(sigmas.begin(), sigmas.end());
        kappas_a = Array(sigmas_a.size(), reversion);
    }
    SavedSettings backup;
    Date evalDate;
    Handle<YieldTermStructure> yts;
    boost::shared_ptr<IborIndex> euribor6m;
    Date effectiveDate, startDate, maturityDate;
    Schedule fixedSchedule, floatingSchedule;
    boost::shared_ptr<VanillaSwap> underlying;
    std::vector<Date> exerciseDates, stepDates;
    std::vector<Real> sigmas;
    boost::shared_ptr<Exercise> exercise;
    boost::shared_ptr<Swaption> swaption;
    Array stepTimes_a, sigmas_a, kappas_a;
    Real reversion;
}; // BermudanTestData

} // anonymous namespace

namespace testsuite {

void CrossAssetModelTest::testBermudanLgm1fGsr() {

    BOOST_TEST_MESSAGE("Testing consistency of Bermudan swaption pricing in "
                       "LGM 1F and GSR models...");

    BermudanTestData d;

    // we use the Hull White adaptor for the LGM parametrization
    // which should lead to equal Bermudan swaption prices
    boost::shared_ptr<IrLgm1fParametrization> lgm_p = boost::make_shared<IrLgm1fPiecewiseConstantHullWhiteAdaptor>(
        EURCurrency(), d.yts, d.stepTimes_a, d.sigmas_a, d.stepTimes_a, d.kappas_a);

    // fix any T forward measure
    boost::shared_ptr<Gsr> gsr = boost::make_shared<Gsr>(d.yts, d.stepDates, d.sigmas, d.reversion, 50.0);

    boost::shared_ptr<LinearGaussMarkovModel> lgm = boost::make_shared<LinearGaussMarkovModel>(lgm_p);

    boost::shared_ptr<Gaussian1dModel> lgm_g1d = boost::make_shared<Gaussian1dCrossAssetAdaptor>(lgm);

    boost::shared_ptr<PricingEngine> swaptionEngineGsr =
        boost::make_shared<Gaussian1dSwaptionEngine>(gsr, 64, 7.0, true, false);

    boost::shared_ptr<PricingEngine> swaptionEngineLgm =
        boost::make_shared<Gaussian1dSwaptionEngine>(lgm_g1d, 64, 7.0, true, false);

    boost::shared_ptr<PricingEngine> swaptionEngineLgm2 =
        boost::make_shared<NumericLgmSwaptionEngine>(lgm, 7.0, 16, 7.0, 32);

    d.swaption->setPricingEngine(swaptionEngineGsr);
    Real npvGsr = d.swaption->NPV();
    d.swaption->setPricingEngine(swaptionEngineLgm);
    Real npvLgm = d.swaption->NPV();
    d.swaption->setPricingEngine(swaptionEngineLgm2);
    Real npvLgm2 = d.swaption->NPV();

    Real tol = 0.2E-4; // basis point tolerance

    if (std::fabs(npvGsr - npvLgm) > tol)
        BOOST_ERROR("Failed to verify consistency of Bermudan swaption price "
                    "in IrLgm1f / Gaussian1d adaptor engine ("
                    << npvLgm << ") and Gsr (" << npvGsr << ") models, tolerance is " << tol);

    if (std::fabs(npvGsr - npvLgm2) > tol)
        BOOST_ERROR("Failed to verify consistency of Bermudan swaption price "
                    "in IrLgm1f / Numeric LGM engine ("
                    << npvLgm2 << ") and Gsr (" << npvGsr << ") models, tolerance is " << tol);
}

void CrossAssetModelTest::testBermudanLgmInvariances() {

    BOOST_TEST_MESSAGE("Testing LGM model invariances for Bermudan "
                       "swaption pricing...");

    BermudanTestData d;

    boost::shared_ptr<IrLgm1fParametrization> lgm_p2 = boost::make_shared<IrLgm1fPiecewiseConstantHullWhiteAdaptor>(
        EURCurrency(), d.yts, d.stepTimes_a, d.sigmas_a, d.stepTimes_a, d.kappas_a);

    boost::shared_ptr<LinearGaussMarkovModel> lgm2 = boost::make_shared<LinearGaussMarkovModel>(lgm_p2);

    boost::shared_ptr<Gaussian1dModel> lgm_g1d2 = boost::make_shared<Gaussian1dCrossAssetAdaptor>(lgm2);

    boost::shared_ptr<PricingEngine> swaptionEngineLgm2 =
        boost::make_shared<Gaussian1dSwaptionEngine>(lgm_g1d2, 64, 7.0, true, false);

    d.swaption->setPricingEngine(swaptionEngineLgm2);
    Real npvLgm = d.swaption->NPV();

    lgm_p2->shift() = -5.0;
    lgm_p2->scaling() = 3.0;

    // parametrizations are not observed, so we have to call update ourselves
    lgm2->update();

    Real npvLgm2 = d.swaption->NPV();

    Real tol = 1.0E-5;

    if (std::fabs(npvLgm - npvLgm2) > tol)
        BOOST_ERROR("Failed to verify consistency of Bermudan swaption price "
                    "under LGM model invariances, difference is "
                    << (npvLgm - npvLgm2));

} // testBermudanLgm1fGsr

void CrossAssetModelTest::testNonstandardBermudanSwaption() {

    BOOST_TEST_MESSAGE("Testing numeric LGM swaption engine for non-standard swaption...");

    BermudanTestData d;

    boost::shared_ptr<NonstandardSwaption> ns_swaption = boost::make_shared<NonstandardSwaption>(*d.swaption);

    boost::shared_ptr<IrLgm1fParametrization> lgm_p = boost::make_shared<IrLgm1fPiecewiseConstantHullWhiteAdaptor>(
        EURCurrency(), d.yts, d.stepTimes_a, d.sigmas_a, d.stepTimes_a, d.kappas_a);

    boost::shared_ptr<LinearGaussMarkovModel> lgm = boost::make_shared<LinearGaussMarkovModel>(lgm_p);

    boost::shared_ptr<PricingEngine> engine = boost::make_shared<NumericLgmSwaptionEngine>(lgm, 7.0, 16, 7.0, 32);
    boost::shared_ptr<PricingEngine> ns_engine =
        boost::make_shared<NumericLgmNonstandardSwaptionEngine>(lgm, 7.0, 16, 7.0, 32);

    d.swaption->setPricingEngine(engine);
    ns_swaption->setPricingEngine(ns_engine);

    Real npv = d.swaption->NPV();
    Real ns_npv = d.swaption->NPV();

    Real tol = 1.0E-12;
    if (std::fabs(npv - ns_npv) >= tol)
        BOOST_ERROR("Failed to verify consistency of Bermudan swaption price ("
                    << npv << ") and Bermudan nonstandard swaption price (" << ns_npv << "), difference is "
                    << (npv - ns_npv) << ", tolerance is " << tol);
} // testNonstandardBermudanSwaption

void CrossAssetModelTest::testLgm1fCalibration() {

    BOOST_TEST_MESSAGE("Testing calibration of LGM 1F model (analytic engine) "
                       "against GSR parameters...");

    // for fixed kappa != 0.0 we calibrate sigma via
    // the Hull White Adaptor

    SavedSettings backup;

    Date evalDate(12, January, 2015);
    Settings::instance().evaluationDate() = evalDate;
    Handle<YieldTermStructure> yts(boost::make_shared<FlatForward>(evalDate, 0.02, Actual365Fixed()));
    boost::shared_ptr<IborIndex> euribor6m = boost::make_shared<Euribor>(6 * Months, yts);

    // coterminal basket 1y-9y, 2y-8y, ... 9y-1y

    std::vector<boost::shared_ptr<CalibrationHelper> > basket;
    Real impliedVols[] = { 0.4, 0.39, 0.38, 0.35, 0.35, 0.34, 0.33, 0.32, 0.31 };
    std::vector<Date> expiryDates;

    for (Size i = 0; i < 9; ++i) {
        boost::shared_ptr<CalibrationHelper> helper = boost::make_shared<SwaptionHelper>(
            (i + 1) * Years, (9 - i) * Years, Handle<Quote>(boost::make_shared<SimpleQuote>(impliedVols[i])), euribor6m,
            1 * Years, Thirty360(), Actual360(), yts);
        basket.push_back(helper);
        expiryDates.push_back(
            boost::static_pointer_cast<SwaptionHelper>(helper)->swaption()->exercise()->dates().back());
    }

    std::vector<Date> stepDates(expiryDates.begin(), expiryDates.end() - 1);

    Array stepTimes_a(stepDates.size());
    for (Size i = 0; i < stepDates.size(); ++i) {
        stepTimes_a[i] = yts->timeFromReference(stepDates[i]);
    }

    Real kappa = 0.05;

    std::vector<Real> gsrInitialSigmas(stepDates.size() + 1, 0.0050);
    std::vector<Real> lgmInitialSigmas2(stepDates.size() + 1, 0.0050);

    Array lgmInitialSigmas2_a(lgmInitialSigmas2.begin(), lgmInitialSigmas2.end());
    Array kappas_a(lgmInitialSigmas2_a.size(), kappa);

    boost::shared_ptr<IrLgm1fParametrization> lgm_p = boost::make_shared<IrLgm1fPiecewiseConstantHullWhiteAdaptor>(
        EURCurrency(), yts, stepTimes_a, lgmInitialSigmas2_a, stepTimes_a, kappas_a);

    // fix any T forward measure
    boost::shared_ptr<Gsr> gsr = boost::make_shared<Gsr>(yts, stepDates, gsrInitialSigmas, kappa, 50.0);

    boost::shared_ptr<LinearGaussMarkovModel> lgm = boost::make_shared<LinearGaussMarkovModel>(lgm_p);

    boost::shared_ptr<PricingEngine> swaptionEngineGsr =
        boost::make_shared<Gaussian1dSwaptionEngine>(gsr, 64, 7.0, true, false);

    boost::shared_ptr<PricingEngine> swaptionEngineLgm = boost::make_shared<AnalyticLgmSwaptionEngine>(lgm);

    // calibrate GSR

    LevenbergMarquardt lm(1E-8, 1E-8, 1E-8);
    EndCriteria ec(1000, 500, 1E-8, 1E-8, 1E-8);

    for (Size i = 0; i < basket.size(); ++i) {
        basket[i]->setPricingEngine(swaptionEngineGsr);
    }

    gsr->calibrateVolatilitiesIterative(basket, lm, ec);

    Array gsrSigmas = gsr->volatility();

    // calibrate LGM

    for (Size i = 0; i < basket.size(); ++i) {
        basket[i]->setPricingEngine(swaptionEngineLgm);
    }

    lgm->calibrateVolatilitiesIterative(basket, lm, ec);

    Array lgmSigmas = lgm->parametrization()->parameterValues(0);

    Real tol0 = 1E-8;
    Real tol = 2E-5;

    for (Size i = 0; i < gsrSigmas.size(); ++i) {
        // check calibration itself, we should match the market prices
        // rather exactly (note that this tests the lgm calibration,
        // not the gsr calibration)
        if (std::fabs(basket[i]->modelValue() - basket[i]->marketValue()) > tol0)
            BOOST_ERROR("Failed to calibrate to market swaption #"
                        << i << ", market price is " << basket[i]->marketValue() << " while model price is "
                        << basket[i]->modelValue());
        // compare calibrated model parameters
        if (std::fabs(gsrSigmas[i] - lgmSigmas[i]) > tol)
            BOOST_ERROR("Failed to verify LGM's sigma from Hull White adaptor (#"
                        << i << "), which is " << lgmSigmas[i] << " while GSR's sigma is " << gsrSigmas[i] << ")");
    }

    // calibrate LGM as component of CrossAssetModel

    // create a second set of parametrization ...
    boost::shared_ptr<IrLgm1fParametrization> lgm_p21 = boost::make_shared<IrLgm1fPiecewiseConstantHullWhiteAdaptor>(
        USDCurrency(), yts, stepTimes_a, lgmInitialSigmas2_a, stepTimes_a, kappas_a);
    boost::shared_ptr<IrLgm1fParametrization> lgm_p22 = boost::make_shared<IrLgm1fPiecewiseConstantHullWhiteAdaptor>(
        EURCurrency(), yts, stepTimes_a, lgmInitialSigmas2_a, stepTimes_a, kappas_a);

    // ... and a fx parametrization ...
    Array notimes_a(0);
    Array sigma_a(1, 0.10);
    boost::shared_ptr<FxBsParametrization> fx_p = boost::make_shared<FxBsPiecewiseConstantParametrization>(
        EURCurrency(), Handle<Quote>(boost::make_shared<SimpleQuote>(1.00)), notimes_a, sigma_a);

    // ... and set up an crossasset model with USD as domestic currency ...
    std::vector<boost::shared_ptr<Parametrization> > parametrizations;
    parametrizations.push_back(lgm_p21);
    parametrizations.push_back(lgm_p22);
    parametrizations.push_back(fx_p);
    Matrix rho(3, 3, 0.0);
    rho[0][0] = rho[1][1] = rho[2][2] = 1.0;
    boost::shared_ptr<CrossAssetModel> xmodel =
        boost::make_shared<CrossAssetModel>(parametrizations, rho, SalvagingAlgorithm::None);

    // .. whose EUR component we calibrate as before and compare the
    // result against the 1d case and as well check that the USD
    // component was not touched by the calibration.

    boost::shared_ptr<PricingEngine> swaptionEngineLgm2 = boost::make_shared<AnalyticLgmSwaptionEngine>(xmodel, 1);

    for (Size i = 0; i < basket.size(); ++i) {
        basket[i]->setPricingEngine(swaptionEngineLgm2);
    }

    xmodel->calibrateIrLgm1fVolatilitiesIterative(1, basket, lm, ec);

    Array lgmSigmas2eur = xmodel->irlgm1f(1)->parameterValues(0);
    Array lgmSigmas2usd = xmodel->irlgm1f(0)->parameterValues(0);

    for (Size i = 0; i < gsrSigmas.size(); ++i) {
        // compare calibrated model parameters against 1d calibration before
        if (!close_enough(lgmSigmas2eur[i], lgmSigmas[i]))
            BOOST_ERROR("Failed to verify crossasset LGM1F component calibration "
                        "at parameter #"
                        << i << " against 1d calibration, which is " << lgmSigmas2eur[i] << " while 1d calibration was "
                        << lgmSigmas[i] << ")");
        // compare usd component against start values (since it was not
        // calibrated, so should not have changed)
        if (!close_enough(lgmSigmas2usd[i], lgmInitialSigmas2[i]))
            BOOST_ERROR("Non calibrated crossasset LGM1F component was changed by "
                        "other's component calibration at #"
                        << i << ", the new value is " << lgmSigmas2usd[i] << " while the initial value was "
                        << lgmInitialSigmas2[i]);
    }

} // testLgm1fCalibration

void CrossAssetModelTest::testCcyLgm3fForeignPayouts() {

    BOOST_TEST_MESSAGE("Testing pricing of foreign payouts under domestic "
                       "measure in Ccy LGM 3F model...");

    SavedSettings backup;

    Date referenceDate(30, July, 2015);

    Settings::instance().evaluationDate() = referenceDate;

    Handle<YieldTermStructure> eurYts(boost::make_shared<FlatForward>(referenceDate, 0.02, Actual365Fixed()));

    Handle<YieldTermStructure> usdYts(boost::make_shared<FlatForward>(referenceDate, 0.05, Actual365Fixed()));

    // use different grids for the EUR and USD  models and the FX volatility
    // process to test the piecewise numerical integration ...

    std::vector<Date> volstepdatesEur, volstepdatesUsd, volstepdatesFx;

    volstepdatesEur.push_back(Date(15, July, 2016));
    volstepdatesEur.push_back(Date(15, July, 2017));
    volstepdatesEur.push_back(Date(15, July, 2018));
    volstepdatesEur.push_back(Date(15, July, 2019));
    volstepdatesEur.push_back(Date(15, July, 2020));

    volstepdatesUsd.push_back(Date(13, April, 2016));
    volstepdatesUsd.push_back(Date(13, September, 2016));
    volstepdatesUsd.push_back(Date(13, April, 2017));
    volstepdatesUsd.push_back(Date(13, September, 2017));
    volstepdatesUsd.push_back(Date(13, April, 2018));
    volstepdatesUsd.push_back(Date(15, July, 2018)); // shared with EUR
    volstepdatesUsd.push_back(Date(13, April, 2019));
    volstepdatesUsd.push_back(Date(13, September, 2019));

    volstepdatesFx.push_back(Date(15, July, 2016)); // shared with EUR
    volstepdatesFx.push_back(Date(15, October, 2016));
    volstepdatesFx.push_back(Date(15, May, 2017));
    volstepdatesFx.push_back(Date(13, September, 2017)); // shared with USD
    volstepdatesFx.push_back(Date(15, July, 2018));      //  shared with EUR and USD

    std::vector<Real> eurVols, usdVols, fxVols;

    for (Size i = 0; i < volstepdatesEur.size() + 1; ++i) {
        eurVols.push_back(0.0050 + (0.0080 - 0.0050) * std::exp(-0.3 * static_cast<double>(i)));
    }
    for (Size i = 0; i < volstepdatesUsd.size() + 1; ++i) {
        usdVols.push_back(0.0030 + (0.0110 - 0.0030) * std::exp(-0.3 * static_cast<double>(i)));
    }
    for (Size i = 0; i < volstepdatesFx.size() + 1; ++i) {
        fxVols.push_back(0.15 + (0.20 - 0.15) * std::exp(-0.3 * static_cast<double>(i)));
    }

    Array alphaTimesEur(volstepdatesEur.size()), alphaEur(eurVols.begin(), eurVols.end()), kappaTimesEur(0),
        kappaEur(1, 0.02);
    Array alphaTimesUsd(volstepdatesUsd.size()), alphaUsd(usdVols.begin(), usdVols.end()), kappaTimesUsd(0),
        kappaUsd(1, 0.04);
    Array fxTimes(volstepdatesFx.size()), fxSigmas(fxVols.begin(), fxVols.end());

    for (Size i = 0; i < alphaTimesEur.size(); ++i) {
        alphaTimesEur[i] = eurYts->timeFromReference(volstepdatesEur[i]);
    }
    for (Size i = 0; i < alphaTimesUsd.size(); ++i) {
        alphaTimesUsd[i] = eurYts->timeFromReference(volstepdatesUsd[i]);
    }
    for (Size i = 0; i < fxTimes.size(); ++i) {
        fxTimes[i] = eurYts->timeFromReference(volstepdatesFx[i]);
    }

    boost::shared_ptr<IrLgm1fParametrization> eurLgmParam = boost::make_shared<IrLgm1fPiecewiseConstantParametrization>(
        EURCurrency(), eurYts, alphaTimesEur, alphaEur, kappaTimesEur, kappaEur);

    boost::shared_ptr<IrLgm1fParametrization> usdLgmParam = boost::make_shared<IrLgm1fPiecewiseConstantParametrization>(
        USDCurrency(), usdYts, alphaTimesUsd, alphaUsd, kappaTimesUsd, kappaUsd);

    // USD per EUR (foreign per domestic)
    Handle<Quote> usdEurSpotToday(boost::make_shared<SimpleQuote>(0.90));

    boost::shared_ptr<FxBsParametrization> fxUsdEurBsParam =
        boost::make_shared<FxBsPiecewiseConstantParametrization>(USDCurrency(), usdEurSpotToday, fxTimes, fxSigmas);

    std::vector<boost::shared_ptr<Parametrization> > singleModels;
    singleModels.push_back(eurLgmParam);
    singleModels.push_back(usdLgmParam);
    singleModels.push_back(fxUsdEurBsParam);

    boost::shared_ptr<CrossAssetModel> ccLgm = boost::make_shared<CrossAssetModel>(singleModels);

    Size eurIdx = ccLgm->ccyIndex(EURCurrency());
    Size usdIdx = ccLgm->ccyIndex(USDCurrency());
    Size eurUsdIdx = usdIdx - 1;

    ccLgm->correlation(IR, eurIdx, IR, usdIdx, -0.2);
    ccLgm->correlation(IR, eurIdx, FX, eurUsdIdx, 0.8);
    ccLgm->correlation(IR, usdIdx, FX, eurUsdIdx, -0.5);

    boost::shared_ptr<LinearGaussMarkovModel> eurLgm = boost::make_shared<LinearGaussMarkovModel>(eurLgmParam);
    boost::shared_ptr<LinearGaussMarkovModel> usdLgm = boost::make_shared<LinearGaussMarkovModel>(usdLgmParam);

    boost::shared_ptr<StochasticProcess> process = ccLgm->stateProcess(CrossAssetStateProcess::exact);
    boost::shared_ptr<StochasticProcess> usdProcess = usdLgm->stateProcess();

    // path generation

    Size n = 500000; // number of paths
    Size seed = 121; // seed
    // maturity of test payoffs
    Time T = 5.0;
    // take large steps, but not only one (since we are testing)
    Size steps = static_cast<Size>(T * 2.0);
    TimeGrid grid(T, steps);
    PseudoRandom::rsg_type sg2 = PseudoRandom::make_sequence_generator(steps, seed);

    MultiPathGeneratorMersenneTwister pg(process, grid, seed, false);
    PathGenerator<PseudoRandom::rsg_type> pg2(usdProcess, grid, sg2, false);

    // test
    // 1 deterministic USD cashflow under EUR numeraire vs. price on USD curve
    // 2 zero bond option USD under EUR numeraire vs. USD numeraire
    // 3 fx option USD-EUR under EUR numeraire vs. analytical price

    accumulator_set<double, stats<tag::mean, tag::error_of<tag::mean> > > stat1, stat2a, stat2b, stat3;

    // same for paths2 since shared time grid
    for (Size j = 0; j < n; ++j) {
        Sample<MultiPath> path = pg.next();
        Sample<Path> path2 = pg2.next();
        Size l = path.value[0].length() - 1;
        Real fx = std::exp(path.value[2][l]);
        Real zeur = path.value[0][l];
        Real zusd = path.value[1][l];
        Real zusd2 = path2.value[l];

        // 1 USD paid at T deflated with EUR numeraire
        stat1(1.0 * fx / eurLgm->numeraire(T, zeur));

        // 2 USD zero bond option at T on P(T,T+10) strike 0.5 ...
        // ... under EUR numeraire ...
        Real zbOpt = std::max(usdLgm->discountBond(T, T + 10.0, zusd) - 0.5, 0.0);
        stat2a(zbOpt * fx / eurLgm->numeraire(T, zeur));
        // ... and under USD numeraire ...
        Real zbOpt2 = std::max(usdLgm->discountBond(T, T + 10.0, zusd2) - 0.5, 0.0);
        stat2b(zbOpt2 / usdLgm->numeraire(T, zusd2));

        // 3 USD-EUR fx option @0.9
        stat3(std::max(fx - 0.9, 0.0) / eurLgm->numeraire(T, zeur));
    }

    boost::shared_ptr<VanillaOption> fxOption =
        boost::make_shared<VanillaOption>(boost::make_shared<PlainVanillaPayoff>(Option::Call, 0.9),
                                          boost::make_shared<EuropeanExercise>(referenceDate + 5 * 365));

    boost::shared_ptr<AnalyticCcLgmFxOptionEngine> ccLgmFxOptionEngine =
        boost::make_shared<AnalyticCcLgmFxOptionEngine>(ccLgm, 0);

    ccLgmFxOptionEngine->cache();

    fxOption->setPricingEngine(ccLgmFxOptionEngine);

    Real npv1 = mean(stat1);
    Real error1 = error_of<tag::mean>(stat1);
    Real expected1 = usdYts->discount(5.0) * usdEurSpotToday->value();
    Real npv2a = mean(stat2a);
    Real error2a = error_of<tag::mean>(stat2a);
    Real npv2b = mean(stat2b) * usdEurSpotToday->value();
    ;
    Real error2b = error_of<tag::mean>(stat2b) * usdEurSpotToday->value();
    Real npv3 = mean(stat3);
    Real error3 = error_of<tag::mean>(stat3);

    // accept this relative difference in error estimates
    Real tolError = 0.2;
    // accept tolErrEst*errorEstimate as absolute difference
    Real tolErrEst = 1.0;

    if (std::fabs((error1 - 4E-4) / 4E-4) > tolError)
        BOOST_ERROR("error estimate deterministic "
                    "cashflow pricing can not be "
                    "reproduced, is "
                    << error1 << ", expected 4E-4, relative tolerance " << tolError);
    if (std::fabs((error2a - 1E-4) / 1E-4) > tolError)
        BOOST_ERROR("error estimate zero bond "
                    "option pricing (foreign measure) can "
                    "not be reproduced, is "
                    << error2a << ", expected 1E-4, relative tolerance " << tolError);
    if (std::fabs((error2b - 7E-5) / 7E-5) > tolError)
        BOOST_ERROR("error estimate zero bond "
                    "option pricing (domestic measure) can "
                    "not be reproduced, is "
                    << error2b << ", expected 7E-5, relative tolerance " << tolError);
    if (std::fabs((error3 - 2.7E-4) / 2.7E-4) > tolError)
        BOOST_ERROR("error estimate fx option pricing can not be reproduced, is "
                    << error3 << ", expected 2.7E-4, relative tolerance " << tolError);

    if (std::fabs(npv1 - expected1) > tolErrEst * error1)
        BOOST_ERROR("can no reproduce deterministic cashflow pricing, is "
                    << npv1 << ", expected " << expected1 << ", tolerance " << tolErrEst << "*" << error1);

    if (std::fabs(npv2a - npv2b) > tolErrEst * std::sqrt(error2a * error2a + error2b * error2b))
        BOOST_ERROR("can no reproduce zero bond option pricing, domestic "
                    "measure result is "
                    << npv2a << ", foreign measure result is " << npv2b << ", tolerance " << tolErrEst << "*"
                    << std::sqrt(error2a * error2a + error2b * error2b));

    if (std::fabs(npv3 - fxOption->NPV()) > tolErrEst * error3)
        BOOST_ERROR("can no reproduce fx option pricing, monte carlo result is "
                    << npv3 << ", analytical pricing result is " << fxOption->NPV() << ", tolerance is " << tolErrEst
                    << "*" << error3);

} // testCcyLgm3fForeignPayouts

namespace {

struct Lgm5fTestData {
    Lgm5fTestData()
        : referenceDate(30, July, 2015), eurYts(boost::make_shared<FlatForward>(referenceDate, 0.02, Actual365Fixed())),
          usdYts(boost::make_shared<FlatForward>(referenceDate, 0.05, Actual365Fixed())),
          gbpYts(boost::make_shared<FlatForward>(referenceDate, 0.04, Actual365Fixed())),
          fxEurUsd(boost::make_shared<SimpleQuote>(0.90)), fxEurGbp(boost::make_shared<SimpleQuote>(1.35)), c(5, 5) {

        Settings::instance().evaluationDate() = referenceDate;
        volstepdates.push_back(Date(15, July, 2016));
        volstepdates.push_back(Date(15, July, 2017));
        volstepdates.push_back(Date(15, July, 2018));
        volstepdates.push_back(Date(15, July, 2019));
        volstepdates.push_back(Date(15, July, 2020));

        volstepdatesFx.push_back(Date(15, July, 2016));
        volstepdatesFx.push_back(Date(15, October, 2016));
        volstepdatesFx.push_back(Date(15, May, 2017));
        volstepdatesFx.push_back(Date(13, September, 2017));
        volstepdatesFx.push_back(Date(15, July, 2018));

        volsteptimes_a = Array(volstepdates.size());
        volsteptimesFx_a = Array(volstepdatesFx.size());
        for (Size i = 0; i < volstepdates.size(); ++i) {
            volsteptimes_a[i] = eurYts->timeFromReference(volstepdates[i]);
        }
        for (Size i = 0; i < volstepdatesFx.size(); ++i) {
            volsteptimesFx_a[i] = eurYts->timeFromReference(volstepdatesFx[i]);
        }

        for (Size i = 0; i < volstepdates.size() + 1; ++i) {
            eurVols.push_back(0.0050 + (0.0080 - 0.0050) * std::exp(-0.3 * static_cast<double>(i)));
        }
        for (Size i = 0; i < volstepdates.size() + 1; ++i) {
            usdVols.push_back(0.0030 + (0.0110 - 0.0030) * std::exp(-0.3 * static_cast<double>(i)));
        }
        for (Size i = 0; i < volstepdates.size() + 1; ++i) {
            gbpVols.push_back(0.0070 + (0.0095 - 0.0070) * std::exp(-0.3 * static_cast<double>(i)));
        }
        for (Size i = 0; i < volstepdatesFx.size() + 1; ++i) {
            fxSigmasUsd.push_back(0.15 + (0.20 - 0.15) * std::exp(-0.3 * static_cast<double>(i)));
        }
        for (Size i = 0; i < volstepdatesFx.size() + 1; ++i) {
            fxSigmasGbp.push_back(0.10 + (0.15 - 0.10) * std::exp(-0.3 * static_cast<double>(i)));
        }
        eurVols_a = Array(eurVols.begin(), eurVols.end());
        usdVols_a = Array(usdVols.begin(), usdVols.end());
        gbpVols_a = Array(gbpVols.begin(), gbpVols.end());
        fxSigmasUsd_a = Array(fxSigmasUsd.begin(), fxSigmasUsd.end());
        fxSigmasGbp_a = Array(fxSigmasGbp.begin(), fxSigmasGbp.end());

        notimes_a = Array(0);
        eurKappa_a = Array(1, 0.02);
        usdKappa_a = Array(1, 0.03);
        gbpKappa_a = Array(1, 0.04);

        eurLgm_p = boost::make_shared<IrLgm1fPiecewiseConstantParametrization>(EURCurrency(), eurYts, volsteptimes_a,
                                                                               eurVols_a, notimes_a, eurKappa_a);
        usdLgm_p = boost::make_shared<IrLgm1fPiecewiseConstantParametrization>(USDCurrency(), usdYts, volsteptimes_a,
                                                                               usdVols_a, notimes_a, usdKappa_a);
        gbpLgm_p = boost::make_shared<IrLgm1fPiecewiseConstantParametrization>(GBPCurrency(), gbpYts, volsteptimes_a,
                                                                               gbpVols_a, notimes_a, gbpKappa_a);

        fxUsd_p = boost::make_shared<FxBsPiecewiseConstantParametrization>(USDCurrency(), fxEurUsd, volsteptimesFx_a,
                                                                           fxSigmasUsd_a);
        fxGbp_p = boost::make_shared<FxBsPiecewiseConstantParametrization>(GBPCurrency(), fxEurGbp, volsteptimesFx_a,
                                                                           fxSigmasGbp_a);

        singleModels.push_back(eurLgm_p);
        singleModels.push_back(usdLgm_p);
        singleModels.push_back(gbpLgm_p);
        singleModels.push_back(fxUsd_p);
        singleModels.push_back(fxGbp_p);

        //     EUR           USD           GBP         FX USD-EUR      FX GBP-EUR
        c[0][0] = 1.0;
        c[0][1] = 0.6;
        c[0][2] = 0.3;
        c[0][3] = 0.2;
        c[0][4] = 0.3; // EUR
        c[1][0] = 0.6;
        c[1][1] = 1.0;
        c[1][2] = 0.1;
        c[1][3] = -0.2;
        c[1][4] = -0.1; // USD
        c[2][0] = 0.3;
        c[2][1] = 0.1;
        c[2][2] = 1.0;
        c[2][3] = 0.0;
        c[2][4] = 0.1; // GBP
        c[3][0] = 0.2;
        c[3][1] = -0.2;
        c[3][2] = 0.0;
        c[3][3] = 1.0;
        c[3][4] = 0.3; // FX USD-EUR
        c[4][0] = 0.3;
        c[4][1] = -0.1;
        c[4][2] = 0.1;
        c[4][3] = 0.3;
        c[4][4] = 1.0; // FX GBP-EUR

        ccLgm = boost::make_shared<CrossAssetModel>(singleModels, c, SalvagingAlgorithm::None);
    }

    SavedSettings backup;
    Date referenceDate;
    Handle<YieldTermStructure> eurYts, usdYts, gbpYts;
    std::vector<Date> volstepdates, volstepdatesFx;
    Array volsteptimes_a, volsteptimesFx_a;
    std::vector<Real> eurVols, usdVols, gbpVols, fxSigmasUsd, fxSigmasGbp;
    Handle<Quote> fxEurUsd, fxEurGbp;
    Array eurVols_a, usdVols_a, gbpVols_a, fxSigmasUsd_a, fxSigmasGbp_a;
    Array notimes_a, eurKappa_a, usdKappa_a, gbpKappa_a;
    boost::shared_ptr<IrLgm1fParametrization> eurLgm_p, usdLgm_p, gbpLgm_p;
    boost::shared_ptr<FxBsParametrization> fxUsd_p, fxGbp_p;
    std::vector<boost::shared_ptr<Parametrization> > singleModels;
    Matrix c;
    boost::shared_ptr<CrossAssetModel> ccLgm;
};

// same as above, with addtional credit names and a different correlation matrix
struct IrFxCrModelTestData {
    IrFxCrModelTestData()
        : referenceDate(30, July, 2015), eurYts(boost::make_shared<FlatForward>(referenceDate, 0.02, Actual365Fixed())),
          usdYts(boost::make_shared<FlatForward>(referenceDate, 0.05, Actual365Fixed())),
          gbpYts(boost::make_shared<FlatForward>(referenceDate, 0.04, Actual365Fixed())),
          fxEurUsd(boost::make_shared<SimpleQuote>(0.90)), fxEurGbp(boost::make_shared<SimpleQuote>(1.35)),
          n1Ts(boost::make_shared<FlatHazardRate>(referenceDate, 0.01, Actual365Fixed())),
          n2Ts(boost::make_shared<FlatHazardRate>(referenceDate, 0.05, Actual365Fixed())),
          n3Ts(boost::make_shared<FlatHazardRate>(referenceDate, 0.10, Actual365Fixed())), n1Alpha(0.01), n1Kappa(0.01),
          n2Alpha(0.015), n2Kappa(0.015), n3Alpha(0.0050), n3Kappa(0.0050), c(8, 8, 0.0) {

        Settings::instance().evaluationDate() = referenceDate;
        volstepdates.push_back(Date(15, July, 2016));
        volstepdates.push_back(Date(15, July, 2017));
        volstepdates.push_back(Date(15, July, 2018));
        volstepdates.push_back(Date(15, July, 2019));
        volstepdates.push_back(Date(15, July, 2020));

        volstepdatesFx.push_back(Date(15, July, 2016));
        volstepdatesFx.push_back(Date(15, October, 2016));
        volstepdatesFx.push_back(Date(15, May, 2017));
        volstepdatesFx.push_back(Date(13, September, 2017));
        volstepdatesFx.push_back(Date(15, July, 2018));

        volsteptimes_a = Array(volstepdates.size());
        volsteptimesFx_a = Array(volstepdatesFx.size());
        for (Size i = 0; i < volstepdates.size(); ++i) {
            volsteptimes_a[i] = eurYts->timeFromReference(volstepdates[i]);
        }
        for (Size i = 0; i < volstepdatesFx.size(); ++i) {
            volsteptimesFx_a[i] = eurYts->timeFromReference(volstepdatesFx[i]);
        }

        for (Size i = 0; i < volstepdates.size() + 1; ++i) {
            eurVols.push_back(0.0050 + (0.0080 - 0.0050) * std::exp(-0.3 * static_cast<double>(i)));
        }
        for (Size i = 0; i < volstepdates.size() + 1; ++i) {
            usdVols.push_back(0.0030 + (0.0110 - 0.0030) * std::exp(-0.3 * static_cast<double>(i)));
        }
        for (Size i = 0; i < volstepdates.size() + 1; ++i) {
            gbpVols.push_back(0.0070 + (0.0095 - 0.0070) * std::exp(-0.3 * static_cast<double>(i)));
        }
        for (Size i = 0; i < volstepdatesFx.size() + 1; ++i) {
            fxSigmasUsd.push_back(0.15 + (0.20 - 0.15) * std::exp(-0.3 * static_cast<double>(i)));
        }
        for (Size i = 0; i < volstepdatesFx.size() + 1; ++i) {
            fxSigmasGbp.push_back(0.10 + (0.15 - 0.10) * std::exp(-0.3 * static_cast<double>(i)));
        }
        eurVols_a = Array(eurVols.begin(), eurVols.end());
        usdVols_a = Array(usdVols.begin(), usdVols.end());
        gbpVols_a = Array(gbpVols.begin(), gbpVols.end());
        fxSigmasUsd_a = Array(fxSigmasUsd.begin(), fxSigmasUsd.end());
        fxSigmasGbp_a = Array(fxSigmasGbp.begin(), fxSigmasGbp.end());

        notimes_a = Array(0);
        eurKappa_a = Array(1, 0.02);
        usdKappa_a = Array(1, 0.03);
        gbpKappa_a = Array(1, 0.04);

        eurLgm_p = boost::make_shared<IrLgm1fPiecewiseConstantParametrization>(EURCurrency(), eurYts, volsteptimes_a,
                                                                               eurVols_a, notimes_a, eurKappa_a);
        usdLgm_p = boost::make_shared<IrLgm1fPiecewiseConstantParametrization>(USDCurrency(), usdYts, volsteptimes_a,
                                                                               usdVols_a, notimes_a, usdKappa_a);
        gbpLgm_p = boost::make_shared<IrLgm1fPiecewiseConstantParametrization>(GBPCurrency(), gbpYts, volsteptimes_a,
                                                                               gbpVols_a, notimes_a, gbpKappa_a);

        fxUsd_p = boost::make_shared<FxBsPiecewiseConstantParametrization>(USDCurrency(), fxEurUsd, volsteptimesFx_a,
                                                                           fxSigmasUsd_a);
        fxGbp_p = boost::make_shared<FxBsPiecewiseConstantParametrization>(GBPCurrency(), fxEurGbp, volsteptimesFx_a,
                                                                           fxSigmasGbp_a);

        // credit
        n1_p = boost::make_shared<CrLgm1fConstantParametrization>(EURCurrency(), n1Ts, n1Alpha, n1Kappa);
        n2_p = boost::make_shared<CrLgm1fConstantParametrization>(EURCurrency(), n2Ts, n2Alpha, n2Kappa);
        n3_p = boost::make_shared<CrLgm1fConstantParametrization>(EURCurrency(), n3Ts, n3Alpha, n3Kappa);

        singleModels.push_back(eurLgm_p);
        singleModels.push_back(usdLgm_p);
        singleModels.push_back(gbpLgm_p);
        singleModels.push_back(fxUsd_p);
        singleModels.push_back(fxGbp_p);
        singleModels.push_back(n1_p);
        singleModels.push_back(n2_p);
        singleModels.push_back(n3_p);

        Real tmp[8][8] = {
            // EUR   USD   GBP    FX1  FX2   N1   N2   N3
            { 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 }, // EUR
            { 0.6, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 }, // USD
            { 0.3, 0.1, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0 }, // GBP
            { 0.2, 0.2, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0 }, // FX1
            { 0.3, 0.1, 0.1, 0.3, 1.0, 0.0, 0.0, 0.0 }, // FX2
            { 0.8, 0.2, 0.1, 0.4, 0.2, 1.0, 0.0, 0.0 }, // N1
            { 0.6, 0.1, 0.2, 0.2, 0.5, 0.5, 1.0, 0.0 }, // N2
            { 0.3, 0.2, 0.1, 0.1, 0.3, 0.4, 0.2, 1.0 }  // N3
        };

        for (Size i = 0; i < 8; ++i) {
            for (Size j = 0; j <= i; ++j) {
                c[i][j] = c[j][i] = tmp[i][j];
            }
        }

        // debug output, to be removed
        // std::clog << "correlation matrix is\n" << c << std::endl;
        // end debug output

        model = boost::make_shared<CrossAssetModel>(singleModels, c, SalvagingAlgorithm::None);
    }

    SavedSettings backup;
    Date referenceDate;
    // ir-fx
    Handle<YieldTermStructure> eurYts, usdYts, gbpYts;
    std::vector<Date> volstepdates, volstepdatesFx;
    Array volsteptimes_a, volsteptimesFx_a;
    std::vector<Real> eurVols, usdVols, gbpVols, fxSigmasUsd, fxSigmasGbp;
    Handle<Quote> fxEurUsd, fxEurGbp;
    Array eurVols_a, usdVols_a, gbpVols_a, fxSigmasUsd_a, fxSigmasGbp_a;
    Array notimes_a, eurKappa_a, usdKappa_a, gbpKappa_a;
    boost::shared_ptr<IrLgm1fParametrization> eurLgm_p, usdLgm_p, gbpLgm_p;
    boost::shared_ptr<FxBsParametrization> fxUsd_p, fxGbp_p;
    // cr
    Handle<DefaultProbabilityTermStructure> n1Ts, n2Ts, n3Ts;
    boost::shared_ptr<CrLgm1fParametrization> n1_p, n2_p, n3_p;
    Real n1Alpha, n1Kappa, n2Alpha, n2Kappa, n3Alpha, n3Kappa;
    // model
    std::vector<boost::shared_ptr<Parametrization> > singleModels;
    Matrix c;
    boost::shared_ptr<CrossAssetModel> model;
};

} // anonymous namespace

void CrossAssetModelTest::testLgm5fFxCalibration() {

    BOOST_TEST_MESSAGE("Testing fx calibration in Ccy LGM 5F model...");

    Lgm5fTestData d;

    // we test the 5f model against the 3f model eur-gbp
    std::vector<boost::shared_ptr<Parametrization> > singleModelsProjected;
    singleModelsProjected.push_back(d.eurLgm_p);
    singleModelsProjected.push_back(d.gbpLgm_p);
    singleModelsProjected.push_back(d.fxGbp_p);

    Matrix cProjected(3, 3);
    for (Size i = 0, ii = 0; i < 5; ++i) {
        if (i != 0 && i != 3) {
            for (Size j = 0, jj = 0; j < 5; ++j) {
                if (j != 0 && j != 3)
                    cProjected[ii][jj++] = d.c[i][j];
            }
            ++ii;
        }
    }

    boost::shared_ptr<CrossAssetModel> ccLgmProjected =
        boost::make_shared<CrossAssetModel>(singleModelsProjected, cProjected, SalvagingAlgorithm::None);

    boost::shared_ptr<AnalyticCcLgmFxOptionEngine> ccLgmFxOptionEngineUsd =
        boost::make_shared<AnalyticCcLgmFxOptionEngine>(d.ccLgm, 0);

    boost::shared_ptr<AnalyticCcLgmFxOptionEngine> ccLgmFxOptionEngineGbp =
        boost::make_shared<AnalyticCcLgmFxOptionEngine>(d.ccLgm, 1);

    boost::shared_ptr<AnalyticCcLgmFxOptionEngine> ccLgmProjectedFxOptionEngineGbp =
        boost::make_shared<AnalyticCcLgmFxOptionEngine>(ccLgmProjected, 0);

    ccLgmFxOptionEngineUsd->cache();
    ccLgmFxOptionEngineGbp->cache();
    ccLgmProjectedFxOptionEngineGbp->cache();

    // while the initial fx vol starts at 0.2 for usd and 0.15 for gbp
    // we calibrate to helpers with 0.15 and 0.2 target implied vol
    std::vector<boost::shared_ptr<CalibrationHelper> > helpersUsd, helpersGbp;
    for (Size i = 0; i <= d.volstepdatesFx.size(); ++i) {
        boost::shared_ptr<CalibrationHelper> tmpUsd = boost::make_shared<FxEqOptionHelper>(
            i < d.volstepdatesFx.size() ? d.volstepdatesFx[i] : d.volstepdatesFx.back() + 365, 0.90, d.fxEurUsd,
            Handle<Quote>(boost::make_shared<SimpleQuote>(0.15)), d.ccLgm->irlgm1f(0)->termStructure(),
            d.ccLgm->irlgm1f(1)->termStructure());
        boost::shared_ptr<CalibrationHelper> tmpGbp = boost::make_shared<FxEqOptionHelper>(
            i < d.volstepdatesFx.size() ? d.volstepdatesFx[i] : d.volstepdatesFx.back() + 365, 1.35, d.fxEurGbp,
            Handle<Quote>(boost::make_shared<SimpleQuote>(0.20)), d.ccLgm->irlgm1f(0)->termStructure(),
            d.ccLgm->irlgm1f(2)->termStructure());
        tmpUsd->setPricingEngine(ccLgmFxOptionEngineUsd);
        tmpGbp->setPricingEngine(ccLgmFxOptionEngineGbp);
        helpersUsd.push_back(tmpUsd);
        helpersGbp.push_back(tmpGbp);
    }

    LevenbergMarquardt lm(1E-8, 1E-8, 1E-8);
    EndCriteria ec(1000, 500, 1E-8, 1E-8, 1E-8);

    // calibrate USD-EUR FX volatility
    d.ccLgm->calibrateBsVolatilitiesIterative(CrossAssetModelTypes::FX, 0, helpersUsd, lm, ec);
    // calibrate GBP-EUR FX volatility
    d.ccLgm->calibrateBsVolatilitiesIterative(CrossAssetModelTypes::FX, 1, helpersGbp, lm, ec);

    Real tol = 1E-6;
    for (Size i = 0; i < helpersUsd.size(); ++i) {
        Real market = helpersUsd[i]->marketValue();
        Real model = helpersUsd[i]->modelValue();
        Real calibratedVol = d.ccLgm->fxbs(0)->parameterValues(0)[i];
        if (std::fabs(market - model) > tol) {
            BOOST_ERROR("calibration for fx option helper #" << i << " (USD) failed, market premium is " << market
                                                             << " while model premium is " << model);
        }
        // the stochastic rates produce some noise, but do not have a huge
        // impact on the effective volatility, so we check that they are
        // in line with a cached example (note that the analytic fx option
        // pricing engine was checked against MC in another test case)
        if (std::fabs(calibratedVol - 0.143) > 0.01) {
            BOOST_ERROR("calibrated fx volatility #" << i << " (USD) seems off, expected to be 0.143 +- 0.01, but is "
                                                     << calibratedVol);
        }
    }
    for (Size i = 0; i < helpersGbp.size(); ++i) {
        Real market = helpersGbp[i]->marketValue();
        Real model = helpersGbp[i]->modelValue();
        Real calibratedVol = d.ccLgm->fxbs(1)->parameterValues(0)[i];
        if (std::fabs(market - model) > tol)
            BOOST_ERROR("calibration for fx option helper #" << i << " (GBP) failed, market premium is " << market
                                                             << " while model premium is " << model);
        // see above
        if (std::fabs(calibratedVol - 0.193) > 0.01)
            BOOST_ERROR("calibrated fx volatility #" << i << " (USD) seems off, expected to be 0.193 +- 0.01, but is "
                                                     << calibratedVol);
    }

    // calibrate the projected model

    for (Size i = 0; i < helpersGbp.size(); ++i) {
        helpersGbp[i]->setPricingEngine(ccLgmProjectedFxOptionEngineGbp);
    }

    ccLgmProjected->calibrateBsVolatilitiesIterative(CrossAssetModelTypes::FX, 0, helpersGbp, lm, ec);

    for (Size i = 0; i < helpersGbp.size(); ++i) {
        Real fullModelVol = d.ccLgm->fxbs(1)->parameterValues(0)[i];
        Real projectedModelVol = ccLgmProjected->fxbs(0)->parameterValues(0)[i];
        if (std::fabs(fullModelVol - projectedModelVol) > tol)
            BOOST_ERROR("calibrated fx volatility of full model @"
                        << i << " (" << fullModelVol << ") is inconsistent with that of the projected model ("
                        << projectedModelVol << ")");
    }

} // testLgm5fFxCalibration

void CrossAssetModelTest::testLgm5fFullCalibration() {

    BOOST_TEST_MESSAGE("Testing full calibration of Ccy LGM 5F model...");

    Lgm5fTestData d;

    // calibration baskets

    std::vector<boost::shared_ptr<CalibrationHelper> > basketEur, basketUsd, basketGbp, basketEurUsd, basketEurGbp;

    boost::shared_ptr<IborIndex> euribor6m = boost::make_shared<Euribor>(6 * Months, d.eurYts);
    boost::shared_ptr<IborIndex> usdLibor3m = boost::make_shared<USDLibor>(3 * Months, d.usdYts);
    boost::shared_ptr<IborIndex> gbpLibor3m = boost::make_shared<GBPLibor>(3 * Months, d.gbpYts);

    for (Size i = 0; i <= d.volstepdates.size(); ++i) {
        Date tmp = i < d.volstepdates.size() ? d.volstepdates[i] : d.volstepdates.back() + 365;
        // EUR: atm+200bp, 150bp normal vol
        basketEur.push_back(boost::shared_ptr<SwaptionHelper>(new SwaptionHelper(
            tmp, 10 * Years, Handle<Quote>(boost::make_shared<SimpleQuote>(0.015)), euribor6m, 1 * Years, Thirty360(),
            Actual360(), d.eurYts, CalibrationHelper::RelativePriceError, 0.04, 1.0, Normal)));
        // USD: atm, 20%, lognormal vol
        basketUsd.push_back(boost::shared_ptr<SwaptionHelper>(new SwaptionHelper(
            tmp, 10 * Years, Handle<Quote>(boost::make_shared<SimpleQuote>(0.30)), usdLibor3m, 1 * Years, Thirty360(),
            Actual360(), d.usdYts, CalibrationHelper::RelativePriceError, Null<Real>(), 1.0, ShiftedLognormal, 0.0)));
        // GBP: atm-200bp, 10%, shifted lognormal vol with shift = 2%
        basketGbp.push_back(boost::shared_ptr<SwaptionHelper>(new SwaptionHelper(
            tmp, 10 * Years, Handle<Quote>(boost::make_shared<SimpleQuote>(0.30)), gbpLibor3m, 1 * Years, Thirty360(),
            Actual360(), d.usdYts, CalibrationHelper::RelativePriceError, 0.02, 1.0, ShiftedLognormal, 0.02)));
    }

    for (Size i = 0; i < d.volstepdatesFx.size(); ++i) {
        Date tmp = i < d.volstepdatesFx.size() ? d.volstepdatesFx[i] : d.volstepdatesFx.back() + 365;
        // EUR-USD: atm, 30% (lognormal) vol
        basketEurUsd.push_back(boost::make_shared<FxEqOptionHelper>(
            tmp, Null<Real>(), d.fxEurUsd, Handle<Quote>(boost::make_shared<SimpleQuote>(0.20)), d.eurYts, d.usdYts,
            CalibrationHelper::RelativePriceError));
        // EUR-GBP: atm, 10% (lognormal) vol
        basketEurGbp.push_back(boost::make_shared<FxEqOptionHelper>(
            tmp, Null<Real>(), d.fxEurGbp, Handle<Quote>(boost::make_shared<SimpleQuote>(0.20)), d.eurYts, d.gbpYts,
            CalibrationHelper::RelativePriceError));
    }

    // pricing engines

    boost::shared_ptr<PricingEngine> eurSwEng = boost::make_shared<AnalyticLgmSwaptionEngine>(d.ccLgm, 0);
    boost::shared_ptr<PricingEngine> usdSwEng = boost::make_shared<AnalyticLgmSwaptionEngine>(d.ccLgm, 1);
    boost::shared_ptr<PricingEngine> gbpSwEng = boost::make_shared<AnalyticLgmSwaptionEngine>(d.ccLgm, 2);

    boost::shared_ptr<AnalyticCcLgmFxOptionEngine> eurUsdFxoEng =
        boost::make_shared<AnalyticCcLgmFxOptionEngine>(d.ccLgm, 0);
    boost::shared_ptr<AnalyticCcLgmFxOptionEngine> eurGbpFxoEng =
        boost::make_shared<AnalyticCcLgmFxOptionEngine>(d.ccLgm, 1);

    eurUsdFxoEng->cache();
    eurGbpFxoEng->cache();

    // assign engines to calibration instruments
    for (Size i = 0; i < basketEur.size(); ++i) {
        basketEur[i]->setPricingEngine(eurSwEng);
    }
    for (Size i = 0; i < basketUsd.size(); ++i) {
        basketUsd[i]->setPricingEngine(usdSwEng);
    }
    for (Size i = 0; i < basketGbp.size(); ++i) {
        basketGbp[i]->setPricingEngine(gbpSwEng);
    }
    for (Size i = 0; i < basketEurUsd.size(); ++i) {
        basketEurUsd[i]->setPricingEngine(eurUsdFxoEng);
    }
    for (Size i = 0; i < basketEurGbp.size(); ++i) {
        basketEurGbp[i]->setPricingEngine(eurGbpFxoEng);
    }

    // calibrate the model

    LevenbergMarquardt lm(1E-8, 1E-8, 1E-8);
    EndCriteria ec(1000, 500, 1E-8, 1E-8, 1E-8);

    d.ccLgm->calibrateIrLgm1fVolatilitiesIterative(0, basketEur, lm, ec);
    d.ccLgm->calibrateIrLgm1fVolatilitiesIterative(1, basketUsd, lm, ec);
    d.ccLgm->calibrateIrLgm1fVolatilitiesIterative(2, basketGbp, lm, ec);

    d.ccLgm->calibrateBsVolatilitiesIterative(CrossAssetModelTypes::FX, 0, basketEurUsd, lm, ec);
    d.ccLgm->calibrateBsVolatilitiesIterative(CrossAssetModelTypes::FX, 1, basketEurGbp, lm, ec);

    // check the results

    Real tol = 1E-6;

    for (Size i = 0; i < basketEur.size(); ++i) {
        Real model = basketEur[i]->modelValue();
        Real market = basketEur[i]->marketValue();
        if (std::abs((model - market) / market) > tol)
            BOOST_ERROR("calibration failed for instrument #"
                        << i << " in EUR basket, model value is " << model << " market value is " << market
                        << " relative error " << std::abs((model - market) / market) << " tolerance " << tol);
    }
    for (Size i = 0; i < basketUsd.size(); ++i) {
        Real model = basketUsd[i]->modelValue();
        Real market = basketUsd[i]->marketValue();
        if (std::abs((model - market) / market) > tol)
            BOOST_ERROR("calibration failed for instrument #"
                        << i << " in USD basket, model value is " << model << " market value is " << market
                        << " relative error " << std::abs((model - market) / market) << " tolerance " << tol);
    }
    for (Size i = 0; i < basketGbp.size(); ++i) {
        Real model = basketGbp[i]->modelValue();
        Real market = basketGbp[i]->marketValue();
        if (std::abs((model - market) / market) > tol)
            BOOST_ERROR("calibration failed for instrument #"
                        << i << " in GBP basket, model value is " << model << " market value is " << market
                        << " relative error " << std::abs((model - market) / market) << " tolerance " << tol);
    }
    for (Size i = 0; i < basketEurUsd.size(); ++i) {
        Real model = basketEurUsd[i]->modelValue();
        Real market = basketEurUsd[i]->marketValue();
        if (std::abs((model - market) / market) > tol)
            BOOST_ERROR("calibration failed for instrument #"
                        << i << " in EUR-USD basket, model value is " << model << " market value is " << market
                        << " relative error " << std::abs((model - market) / market) << " tolerance " << tol);
    }
    for (Size i = 0; i < basketEurUsd.size(); ++i) {
        Real model = basketEurGbp[i]->modelValue();
        Real market = basketEurGbp[i]->marketValue();
        if (std::abs((model - market) / market) > tol)
            BOOST_ERROR("calibration failed for instrument #"
                        << i << " in EUR-GBP basket, model value is " << model << " market value is " << market
                        << " relative error " << std::abs((model - market) / market) << " tolerance " << tol);
    }
}

void CrossAssetModelTest::testLgm5fMoments() {

    BOOST_TEST_MESSAGE("Testing analytic moments vs. Euler and exact discretization "
                       "in Ccy LGM 5F model...");

    Lgm5fTestData d;

    boost::shared_ptr<StochasticProcess> p_exact = d.ccLgm->stateProcess(CrossAssetStateProcess::exact);
    boost::shared_ptr<StochasticProcess> p_euler = d.ccLgm->stateProcess(CrossAssetStateProcess::euler);

    Real T = 10.0;                            // horizon at which we compare the moments
    Size steps = static_cast<Size>(T * 10.0); // number of simulation steps
    Size paths = 25000;                       // number of paths

    Array e_an = p_exact->expectation(0.0, p_exact->initialValues(), T);
    Matrix v_an = p_exact->covariance(0.0, p_exact->initialValues(), T);

    TimeGrid grid(T, steps);

    MultiPathGeneratorSobolBrownianBridge pgen(p_euler, grid);
    MultiPathGeneratorSobolBrownianBridge pgen2(p_exact, grid);

    accumulator_set<double, stats<tag::mean, tag::error_of<tag::mean> > > e_eu[5], e_eu2[5];
    accumulator_set<double, stats<tag::covariance<double, tag::covariate1> > > v_eu[5][5], v_eu2[5][5];

    for (Size i = 0; i < paths; ++i) {
        Sample<MultiPath> path = pgen.next();
        Sample<MultiPath> path2 = pgen2.next();
        for (Size ii = 0; ii < 5; ++ii) {
            Real cii = path.value[ii].back();
            Real cii2 = path2.value[ii].back();
            e_eu[ii](cii);
            e_eu2[ii](cii2);
            for (Size jj = 0; jj <= ii; ++jj) {
                Real cjj = path.value[jj].back();
                v_eu[ii][jj](cii, covariate1 = cjj);
                Real cjj2 = path2.value[jj].back();
                v_eu2[ii][jj](cii2, covariate1 = cjj2);
            }
        }
    }

    Real errTolLd[] = { 0.2E-4, 0.2E-4, 0.2E-4, 10.0E-4, 10.0E-4 };

    for (Size i = 0; i < 5; ++i) {
        // check expectation against analytical calculation (Euler)
        if (std::fabs(mean(e_eu[i]) - e_an[i]) > errTolLd[i]) {
            BOOST_ERROR("analytical expectation for component #"
                        << i << " (" << e_an[i] << ") is inconsistent with numerical value (Euler "
                                                   "discretization, "
                        << mean(e_eu[i]) << "), error is " << e_an[i] - mean(e_eu[i]) << " tolerance is "
                        << errTolLd[i]);
        }
        // check expectation against analytical calculation (exact disc)
        if (std::fabs(mean(e_eu2[i]) - e_an[i]) > errTolLd[i]) {
            BOOST_ERROR("analytical expectation for component #"
                        << i << " (" << e_an[i] << ") is inconsistent with numerical value (Exact "
                                                   "discretization, "
                        << mean(e_eu2[i]) << "), error is " << e_an[i] - mean(e_eu2[i]) << " tolerance is "
                        << errTolLd[i]);
        }
    }

    // we have to deal with different natures of volatility
    // for ir (normal) and fx (ln) so different error
    // tolerances apply
    Real tollNormal = 0.1E-4; // ir-ir
    Real tolMixed = 0.25E-4;  // ir-fx
    Real tolLn = 8.0E-4;      // fx-fx
    Real tol;                 // set below

    for (Size i = 0; i < 5; ++i) {
        for (Size j = 0; j <= i; ++j) {
            if (i < 3) {
                tol = tollNormal;
            } else {
                if (j < 3) {
                    tol = tolMixed;
                } else {
                    tol = tolLn;
                }
            }
            if (std::fabs(covariance(v_eu[i][j]) - v_an[i][j]) > tol) {
                BOOST_ERROR("analytical covariance at ("
                            << i << "," << j << ") (" << v_an[i][j] << ") is inconsistent with numerical "
                                                                       "value (Euler discretization, "
                            << covariance(v_eu[i][j]) << "), error is " << v_an[i][j] - covariance(v_eu[i][j])
                            << " tolerance is " << tol);
            }
            if (std::fabs(covariance(v_eu2[i][j]) - v_an[i][j]) > tol) {
                BOOST_ERROR("analytical covariance at ("
                            << i << "," << j << ") (" << v_an[i][j] << ") is inconsistent with numerical "
                                                                       "value (Exact discretization, "
                            << covariance(v_eu2[i][j]) << "), error is " << v_an[i][j] - covariance(v_eu2[i][j])
                            << " tolerance is " << tol);
            }
        }
    }

} // testLgm5fMoments

void CrossAssetModelTest::testLgmGsrEquivalence() {

    BOOST_TEST_MESSAGE("Testing equivalence of GSR and LGM models...");

    SavedSettings backup;

    Date evalDate(12, January, 2015);
    Settings::instance().evaluationDate() = evalDate;
    Handle<YieldTermStructure> yts(boost::make_shared<FlatForward>(evalDate, 0.02, Actual365Fixed()));

    Real T[] = { 10.0, 20.0, 50.0, 100.0 };
    Real sigma[] = { 0.0050, 0.01, 0.02 };
    Real kappa[] = { -0.02, -0.01, 0.0, 0.03, 0.07 };

    for (Size i = 0; i < LENGTH(T); ++i) {
        for (Size j = 0; j < LENGTH(sigma); ++j) {
            for (Size k = 0; k < LENGTH(kappa); ++k) {

                std::vector<Date> stepDates;
                std::vector<Real> sigmas(1, sigma[j]);

                boost::shared_ptr<Gsr> gsr = boost::make_shared<Gsr>(yts, stepDates, sigmas, kappa[k], T[i]);

                Array stepTimes_a(0);
                Array sigmas_a(1, sigma[j]);
                Array kappas_a(1, kappa[k]);

                // for shift = -H(T) we change the LGM measure to the T forward
                // measure effectively
                Real shift = close_enough(kappa[k], 0.0) ? -T[i] : -(1.0 - std::exp(-kappa[k] * T[i])) / kappa[k];
                boost::shared_ptr<IrLgm1fParametrization> lgm_p =
                    boost::make_shared<IrLgm1fPiecewiseConstantHullWhiteAdaptor>(EURCurrency(), yts, stepTimes_a,
                                                                                 sigmas_a, stepTimes_a, kappas_a);
                lgm_p->shift() = shift;

                boost::shared_ptr<LinearGaussMarkovModel> lgm = boost::make_shared<LinearGaussMarkovModel>(lgm_p);

                boost::shared_ptr<StochasticProcess1D> gsr_process = gsr->stateProcess();
                boost::shared_ptr<StochasticProcess1D> lgm_process = lgm->stateProcess();

                Size N = 10000; // number of paths
                Size seed = 123456;
                Size steps = 1;       // one large step
                Real T2 = T[i] - 5.0; // we check a distribution at this time

                TimeGrid grid(T2, steps);

                PseudoRandom::rsg_type sg = PseudoRandom::make_sequence_generator(steps * 1, seed);
                PathGenerator<PseudoRandom::rsg_type> pgen_gsr(gsr_process, grid, sg, false);
                PathGenerator<PseudoRandom::rsg_type> pgen_lgm(lgm_process, grid, sg, false);

                accumulator_set<double, stats<tag::mean, tag::error_of<tag::mean>, tag::variance> > stat_lgm, stat_gsr;

                Real tol = 1.0E-12;
                for (Size ii = 0; ii < N; ++ii) {
                    Sample<Path> path_lgm = pgen_lgm.next();
                    Sample<Path> path_gsr = pgen_gsr.next();
                    Real yGsr = (path_gsr.value.back() - gsr_process->expectation(0.0, 0.0, T2)) /
                                gsr_process->stdDeviation(0.0, 0.0, T2);
                    Real xLgm = path_lgm.value.back();
                    Real gsrRate = -std::log(gsr->zerobond(T2 + 1.0, T2, yGsr));
                    // it's nice to have uniform interfaces in all models ...
                    Real lgmRate = -std::log(lgm->discountBond(T2, T2 + 1.0, xLgm));
                    stat_gsr(gsrRate);
                    stat_lgm(lgmRate);
                    // check pathwise identity
                    if (std::fabs(gsrRate - lgmRate) >= tol) {
                        BOOST_ERROR("lgm rate (" << lgmRate << ") deviates from gsr rate (" << gsrRate << ") on path #"
                                                 << i);
                    }
                }

                // effectively we are checking a pathwise identity
                // here as well, but the statistics seems to better
                // summarize a possible problem, so we output differences
                // in the mean as well
                if (std::fabs(mean(stat_gsr) - mean(stat_lgm)) > tol ||
                    std::fabs(variance(stat_gsr) - variance(stat_lgm)) > tol) {
                    BOOST_ERROR("failed to verify LGM-GSR equivalence, "
                                "(mean,variance) of zero rate is ("
                                << mean(stat_gsr) << "," << variance(stat_gsr) << ") for GSR, (" << mean(stat_lgm)
                                << "," << variance(stat_lgm) << ") for LGM, for T=" << T[i] << ", sigma=" << sigma[j]
                                << ", kappa=" << kappa[k] << ", shift=" << shift);
                }
            }
        }
    }

} // testLgmGsrEquivalence

void CrossAssetModelTest::testLgmMcWithShift() {
    BOOST_TEST_MESSAGE("Testing LGM1F Monte Carlo simulation with shifted H...");

    // cashflow time
    Real T = 50.0;

    // shift horizons
    Real T_shift[] = { 0.0, 10.0, 20.0, 30.0, 40.0, 50.0 };

    // tolerances for error of mean
    Real eom_tol[] = { 0.17, 0.05, 0.02, 0.01, 0.005, 1.0E-12 };

    Handle<YieldTermStructure> yts(boost::make_shared<FlatForward>(0, NullCalendar(), 0.02, Actual365Fixed()));

    boost::shared_ptr<IrLgm1fParametrization> lgm =
        boost::make_shared<IrLgm1fConstantParametrization>(EURCurrency(), yts, 0.01, 0.01);
    boost::shared_ptr<StochasticProcess> p = boost::make_shared<IrLgm1fStateProcess>(lgm);

    boost::shared_ptr<LinearGaussMarkovModel> model = boost::make_shared<LinearGaussMarkovModel>(lgm);

    Size steps = 1;
    Size paths = 10000;
    Size seed = 42;
    TimeGrid grid(T, steps);

    MultiPathGeneratorMersenneTwister pgen(p, grid, seed, true);

    for (Size ii = 0; ii < LENGTH(T_shift); ++ii) {

        lgm->shift() = -(1.0 - exp(-0.01 * T_shift[ii])) / 0.01;

        accumulator_set<double, stats<tag::mean, tag::error_of<tag::mean> > > e_eu;

        for (Size i = 0; i < paths; ++i) {
            Sample<MultiPath> path = pgen.next();
            Sample<MultiPath> path_a = pgen.next();
            e_eu(1.0 / model->numeraire(T, path.value[0].back()));
            e_eu(1.0 / model->numeraire(T, path_a.value[0].back()));
        }

        Real discount = yts->discount(T);

        if (error_of<tag::mean>(e_eu) / discount > eom_tol[ii]) {
            BOOST_ERROR("estimated error of mean for shifted mc simulation with shift "
                        << T_shift[ii] << " can not be verified (" << error_of<tag::mean>(e_eu) / discount
                        << "), tolerance is 1E-8");
        }

        if (std::fabs(mean(e_eu) / discount - 1.0) > eom_tol[ii]) {
            BOOST_ERROR("estimated error for shifted mc simulation with shift " << T_shift[ii] << " can not "
                                                                                                  "be verified ("
                                                                                << mean(e_eu) / discount - 1.0
                                                                                << "), tolerance is 1E-8");
        }
    }

} // testLgmMcWithShift

void CrossAssetModelTest::testIrFxCrMartingaleProperty() {

    BOOST_TEST_MESSAGE("Testing martingale property in ir-fx-cr model for "
                       "Euler and exact discretizations...");

    IrFxCrModelTestData d;

    boost::shared_ptr<StochasticProcess> process1 = d.model->stateProcess(CrossAssetStateProcess::exact);
    boost::shared_ptr<StochasticProcess> process2 = d.model->stateProcess(CrossAssetStateProcess::euler);

    Size n = 50000;                         // number of paths
    Size seed = 18;                         // rng seed
    Time T = 10.0;                          // maturity of payoff
    Time T2 = 20.0;                         // zerobond maturity
    Size steps = static_cast<Size>(T * 24); // number of steps taken (euler)

    LowDiscrepancy::rsg_type sg1 = LowDiscrepancy::make_sequence_generator(d.model->dimension() * 1, seed);
    LowDiscrepancy::rsg_type sg2 = LowDiscrepancy::make_sequence_generator(d.model->dimension() * steps, seed);

    TimeGrid grid1(T, 1);
    MultiPathGenerator<LowDiscrepancy::rsg_type> pg1(process1, grid1, sg1, false);
    TimeGrid grid2(T, steps);
    MultiPathGenerator<LowDiscrepancy::rsg_type> pg2(process2, grid2, sg2, false);

    accumulator_set<double, stats<tag::mean, tag::error_of<tag::mean> > > eurzb1, usdzb1, gbpzb1, n1eur1, n2usd1,
        n3gbp1;
    accumulator_set<double, stats<tag::mean, tag::error_of<tag::mean> > > eurzb2, usdzb2, gbpzb2, n1eur2, n2usd2,
        n3gbp2;

    for (Size j = 0; j < n; ++j) {
        Sample<MultiPath> path1 = pg1.next();
        Sample<MultiPath> path2 = pg2.next();
        Size l1 = path1.value[0].length() - 1;
        Size l2 = path2.value[0].length() - 1;
        Real zeur1 = path1.value[0][l1];
        Real zusd1 = path1.value[1][l1];
        Real zgbp1 = path1.value[2][l1];
        Real fxusd1 = std::exp(path1.value[3][l1]);
        Real fxgbp1 = std::exp(path1.value[4][l1]);
        Real crzn11 = path1.value[5][l1];
        Real cryn11 = path1.value[6][l1];
        Real crzn21 = path1.value[7][l1];
        Real cryn21 = path1.value[8][l1];
        Real crzn31 = path1.value[9][l1];
        Real cryn31 = path1.value[10][l1];
        Real zeur2 = path2.value[0][l2];
        Real zusd2 = path2.value[1][l2];
        Real zgbp2 = path2.value[2][l2];
        Real fxusd2 = std::exp(path2.value[3][l2]);
        Real fxgbp2 = std::exp(path2.value[4][l2]);
        Real crzn12 = path2.value[5][l2];
        Real cryn12 = path2.value[6][l2];
        Real crzn22 = path2.value[7][l2];
        Real cryn22 = path2.value[8][l2];
        Real crzn32 = path2.value[9][l2];
        Real cryn32 = path2.value[10][l2];

        // EUR zerobond
        eurzb1(d.model->discountBond(0, T, T2, zeur1) / d.model->numeraire(0, T, zeur1));
        // USD zerobond
        usdzb1(d.model->discountBond(1, T, T2, zusd1) * fxusd1 / d.model->numeraire(0, T, zeur1));
        // GBP zerobond
        gbpzb1(d.model->discountBond(2, T, T2, zgbp1) * fxgbp1 / d.model->numeraire(0, T, zeur1));
        // EUR defaultable zerobond for name 1
        std::pair<Real, Real> sn11 = d.model->crlgm1fS(0, 0, T, T2, crzn11, cryn11);
        n1eur1(sn11.first * sn11.second * d.model->discountBond(0, T, T2, zeur1) / d.model->numeraire(0, T, zeur1));
        // USD defaultable zerobond for name 2
        std::pair<Real, Real> sn21 = d.model->crlgm1fS(1, 1, T, T2, crzn21, cryn21);
        n2usd1(sn21.first * sn21.second * d.model->discountBond(1, T, T2, zusd1) * fxusd1 /
               d.model->numeraire(0, T, zeur1));
        // GBP defaultable zerobond for name 3
        std::pair<Real, Real> sn31 = d.model->crlgm1fS(2, 2, T, T2, crzn31, cryn31);
        n3gbp1(sn31.first * sn31.second * d.model->discountBond(2, T, T2, zgbp1) * fxgbp1 /
               d.model->numeraire(0, T, zeur1));

        // EUR zerobond
        eurzb2(d.model->discountBond(0, T, T2, zeur2) / d.model->numeraire(0, T, zeur2));
        // USD zerobond
        usdzb2(d.model->discountBond(1, T, T2, zusd2) * fxusd2 / d.model->numeraire(0, T, zeur2));
        // GBP zerobond
        gbpzb2(d.model->discountBond(2, T, T2, zgbp2) * fxgbp2 / d.model->numeraire(0, T, zeur2));
        // EUR defaultable zerobond for name 1
        std::pair<Real, Real> sn12 = d.model->crlgm1fS(0, 0, T, T2, crzn12, cryn12);
        n1eur2(sn12.first * sn12.second * d.model->discountBond(0, T, T2, zeur2) / d.model->numeraire(0, T, zeur2));
        // USD defaultable zerobond for name 2
        std::pair<Real, Real> sn22 = d.model->crlgm1fS(1, 1, T, T2, crzn22, cryn22);
        n2usd2(sn22.first * sn22.second * d.model->discountBond(1, T, T2, zusd2) * fxusd2 /
               d.model->numeraire(0, T, zeur2));
        // GBP defaultable zerobond for name 3
        std::pair<Real, Real> sn32 = d.model->crlgm1fS(2, 2, T, T2, crzn32, cryn32);
        n3gbp2(sn32.first * sn32.second * d.model->discountBond(2, T, T2, zgbp2) * fxgbp2 /
               d.model->numeraire(0, T, zeur2));
    }

    // debug output, to be removed
    // std::clog << "EXACT:" << std::endl;
    // std::clog << "EUR zb = " << mean(eurzb1) << " +- "
    //           << error_of<tag::mean>(eurzb1) << " vs analytical "
    //           << d.eurYts->discount(T2) << std::endl;
    // std::clog << "USD zb = " << mean(usdzb1) << " +- "
    //           << error_of<tag::mean>(usdzb1) << " vs analytical "
    //           << d.usdYts->discount(T2) * d.fxEurUsd->value() << std::endl;
    // std::clog << "GBP zb = " << mean(gbpzb1) << " +- "
    //           << error_of<tag::mean>(gbpzb1) << " vs analytical "
    //           << d.gbpYts->discount(T2) * d.fxEurGbp->value() << std::endl;
    // std::clog << "N1 zb EUR = " << mean(n1eur1) << " +- "
    //           << error_of<tag::mean>(n1eur1) << " vs analytical "
    //           << d.eurYts->discount(T2) * d.n1Ts->survivalProbability(T2)
    //           << std::endl;
    // std::clog << "N2 zb USD = " << mean(n2usd1) << " +- "
    //           << error_of<tag::mean>(n2usd1) << " vs analytical "
    //           << d.fxEurUsd->value() * d.usdYts->discount(T2) *
    //                  d.n2Ts->survivalProbability(T2)
    //           << std::endl;
    // std::clog << "N3 zb GBP = " << mean(n3gbp1) << " +- "
    //           << error_of<tag::mean>(n3gbp1) << " vs analytical "
    //           << d.fxEurGbp->value() * d.gbpYts->discount(T2) *
    //                  d.n3Ts->survivalProbability(T2)
    //           << std::endl;

    // std::clog << "\nEULER:" << std::endl;
    // std::clog << "EUR zb = " << mean(eurzb2) << " +- "
    //           << error_of<tag::mean>(eurzb2) << " vs analytical "
    //           << d.eurYts->discount(T2) << std::endl;
    // std::clog << "USD zb = " << mean(usdzb2) << " +- "
    //           << error_of<tag::mean>(usdzb2) << " vs analytical "
    //           << d.usdYts->discount(T2) * d.fxEurUsd->value() << std::endl;
    // std::clog << "GBP zb = " << mean(gbpzb2) << " +- "
    //           << error_of<tag::mean>(gbpzb2) << " vs analytical "
    //           << d.gbpYts->discount(T2) * d.fxEurGbp->value() << std::endl;
    // std::clog << "N1 zb EUR = " << mean(n1eur2) << " +- "
    //           << error_of<tag::mean>(n1eur2) << " vs analytical "
    //           << d.eurYts->discount(T2) * d.n1Ts->survivalProbability(T2)
    //           << std::endl;
    // std::clog << "N2 zb USD = " << mean(n2usd2) << " +- "
    //           << error_of<tag::mean>(n2usd2) << " vs analytical "
    //           << d.fxEurUsd->value() * d.usdYts->discount(T2) *
    //                  d.n2Ts->survivalProbability(T2)
    //           << std::endl;
    // std::clog << "N3 zb GBP = " << mean(n3gbp2) << " +- "
    //           << error_of<tag::mean>(n3gbp2) << " vs analytical "
    //           << d.fxEurGbp->value() * d.gbpYts->discount(T2) *
    //                  d.n3Ts->survivalProbability(T2)
    //           << std::endl;
    // end debug output

    Real tol1 = 2.0E-4;  // EXACT
    Real tol2 = 12.0E-4; // EULER

    Real ev = d.eurYts->discount(T2);
    if (std::abs(mean(eurzb1) - ev) > tol1)
        BOOST_FAIL("Martingale test failed for eurzb (exact discr.), excpected " << ev << ", got " << mean(eurzb1)
                                                                                 << ", tolerance " << tol1);
    ev = d.usdYts->discount(T2) * d.fxEurUsd->value();
    if (std::abs(mean(usdzb1) - ev) > tol1)
        BOOST_FAIL("Martingale test failed for eurzb (exact discr.), excpected " << ev << ", got " << mean(usdzb1)
                                                                                 << ", tolerance " << tol1);
    ev = d.gbpYts->discount(T2) * d.fxEurGbp->value();
    if (std::abs(mean(gbpzb1) - ev) > tol1)
        BOOST_FAIL("Martingale test failed for eurzb (exact discr.), excpected " << ev << ", got " << mean(gbpzb1)
                                                                                 << ", tolerance " << tol1);
    ev = d.eurYts->discount(T2) * d.n1Ts->survivalProbability(T2);
    if (std::abs(mean(n1eur1) - ev) > tol1)
        BOOST_FAIL("Martingale test failed for eurzb (exact discr.), excpected " << ev << ", got " << mean(n1eur1)
                                                                                 << ", tolerance " << tol1);
    ev = d.fxEurUsd->value() * d.usdYts->discount(T2) * d.n2Ts->survivalProbability(T2);
    if (std::abs(mean(n2usd1) - ev) > tol1)
        BOOST_FAIL("Martingale test failed for eurzb (exact discr.), excpected " << ev << ", got " << mean(n2usd1)
                                                                                 << ", tolerance " << tol1);
    ev = d.fxEurGbp->value() * d.gbpYts->discount(T2) * d.n3Ts->survivalProbability(T2);
    if (std::abs(mean(n3gbp1) - ev) > tol1)
        BOOST_FAIL("Martingale test failed for eurzb (exact discr.), excpected " << ev << ", got " << mean(n3gbp1)
                                                                                 << ", tolerance " << tol1);

    ev = d.eurYts->discount(T2);
    if (std::abs(mean(eurzb2) - ev) > tol2)
        BOOST_FAIL("Martingale test failed for eurzb (Euler discr.), excpected " << ev << ", got " << mean(eurzb2)
                                                                                 << ", tolerance " << tol2);
    ev = d.usdYts->discount(T2) * d.fxEurUsd->value();
    if (std::abs(mean(usdzb2) - ev) > tol2)
        BOOST_FAIL("Martingale test failed for usdzb (Euler discr.), excpected "
                   << ev << ", got " << mean(usdzb2) << ", tolerance " << tol2 * error_of<tag::mean>(usdzb2));
    ev = d.gbpYts->discount(T2) * d.fxEurGbp->value();
    if (std::abs(mean(gbpzb2) - ev) > tol2)
        BOOST_FAIL("Martingale test failed for gbpzb (Euler discr.), excpected " << ev << ", got " << mean(gbpzb2)
                                                                                 << ", tolerance " << tol2);
    ev = d.eurYts->discount(T2) * d.n1Ts->survivalProbability(T2);
    if (std::abs(mean(n1eur2) - ev) > tol2)
        BOOST_FAIL("Martingale test failed for n1eur (Euler discr.), excpected " << ev << ", got " << mean(n1eur2)
                                                                                 << ", tolerance " << tol2);
    ev = d.fxEurUsd->value() * d.usdYts->discount(T2) * d.n2Ts->survivalProbability(T2);
    if (std::abs(mean(n2usd2) - ev) > tol2)
        BOOST_FAIL("Martingale test failed for n2usd (Euler discr.), excpected " << ev << ", got " << mean(n2usd2)
                                                                                 << ", tolerance " << tol2);
    ev = d.fxEurGbp->value() * d.gbpYts->discount(T2) * d.n3Ts->survivalProbability(T2);
    if (std::abs(mean(n3gbp2) - ev) > tol2)
        BOOST_FAIL("Martingale test failed for n3gbp (Euler discr.), excpected " << ev << ", got " << mean(n3gbp2)
                                                                                 << ", tolerance " << tol2);

} // testIrFxCrMartingaleProperty

void CrossAssetModelTest::testIrFxCrMoments() {

    BOOST_TEST_MESSAGE("Testing analytic moments vs. Euler and exact discretization "
                       "in ir-fx-cr model...");

    IrFxCrModelTestData d;

    boost::shared_ptr<StochasticProcess> p_exact = d.model->stateProcess(CrossAssetStateProcess::exact);
    boost::shared_ptr<StochasticProcess> p_euler = d.model->stateProcess(CrossAssetStateProcess::euler);

    Real T = 10;                            // horizon at which we compare the moments
    Size steps = static_cast<Size>(T * 10); // number of simulation steps (Euler and exact)
    Size paths = 30000;                     // number of paths

    Array e_an = p_exact->expectation(0.0, p_exact->initialValues(), T);
    Matrix v_an = p_exact->covariance(0.0, p_exact->initialValues(), T);

    Size seed = 18;
    TimeGrid grid(T, steps);

    MultiPathGeneratorSobolBrownianBridge pgen(p_euler, grid, SobolBrownianGenerator::Diagonal, seed,
                                               SobolRsg::JoeKuoD7);
    MultiPathGeneratorSobolBrownianBridge pgen2(p_exact, grid, SobolBrownianGenerator::Diagonal, seed,
                                                SobolRsg::JoeKuoD7);

    accumulator_set<double, stats<tag::mean, tag::error_of<tag::mean> > > e_eu[11], e_eu2[11];
    accumulator_set<double, stats<tag::covariance<double, tag::covariate1> > > v_eu[11][11], v_eu2[11][11];

    for (Size i = 0; i < paths; ++i) {
        Sample<MultiPath> path = pgen.next();
        Sample<MultiPath> path2 = pgen2.next();
        for (Size ii = 0; ii < 11; ++ii) {
            Real cii = path.value[ii].back();
            Real cii2 = path2.value[ii].back();
            e_eu[ii](cii);
            e_eu2[ii](cii2);
            for (Size jj = 0; jj <= ii; ++jj) {
                Real cjj = path.value[jj].back();
                v_eu[ii][jj](cii, covariate1 = cjj);
                Real cjj2 = path2.value[jj].back();
                v_eu2[ii][jj](cii2, covariate1 = cjj2);
            }
        }
    }

    // debug, to be removed
    // for (Size i = 0; i < 11; ++i) {
    //     std::clog << "E_" << i << " " << e_an[i] << " " << mean(e_eu[i]) << "
    //     "
    //               << mean(e_eu2[i]) << std::endl;
    // }
    // std::clog << "==================" << std::endl;

    // std::clog << "one step analytical" << std::endl;
    // for (Size i = 0; i < 11; ++i) {
    //     for (Size j = 0; j <= i; ++j) {
    //         std::clog << v_an[i][j] << " ";
    //     }
    //     std::clog << std::endl;
    // }
    // std::clog << "==================" << std::endl;

    // std::clog << "euler" << std::endl;
    // for (Size i = 0; i < 11; ++i) {
    //     for (Size j = 0; j <= i; ++j) {
    //         std::clog << covariance(v_eu[i][j]) << " ";
    //     }
    //     std::clog << std::endl;
    // }
    // std::clog << "==================" << std::endl;

    // std::clog << "exact" << std::endl;
    // for (Size i = 0; i < 11; ++i) {
    //     for (Size j = 0; j <= i; ++j) {
    //         std::clog << covariance(v_eu2[i][j]) << " ";
    //     }
    //     std::clog << std::endl;
    // }
    // std::clog << "==================" << std::endl;

    // end debug

    Real errTolLd[] = { 0.5E-4, 0.5E-4, 0.5E-4, 10.0E-4, 10.0E-4, 0.7E-4, 0.7E-4, 0.7E-4, 0.7E-4, 0.7E-4, 0.7E-4 };

    for (Size i = 0; i < 11; ++i) {
        // check expectation against analytical calculation (Euler)
        if (std::fabs(mean(e_eu[i]) - e_an[i]) > errTolLd[i]) {
            BOOST_ERROR("analytical expectation for component #"
                        << i << " (" << e_an[i] << ") is inconsistent with numerical value (Euler "
                                                   "discretization, "
                        << mean(e_eu[i]) << "), error is " << e_an[i] - mean(e_eu[i]) << " tolerance is "
                        << errTolLd[i]);
        }
        // check expectation against analytical calculation (exact disc)
        if (std::fabs(mean(e_eu2[i]) - e_an[i]) > errTolLd[i]) {
            BOOST_ERROR("analytical expectation for component #"
                        << i << " (" << e_an[i] << ") is inconsistent with numerical value (exact "
                                                   "discretization, "
                        << mean(e_eu2[i]) << "), error is " << e_an[i] - mean(e_eu2[i]) << " tolerance is "
                        << errTolLd[i]);
        }
    }

    // this is a bit rough compared to the more differentiated test
    // of the IR-FX model ...
    Real tol = 10.0E-4;

    for (Size i = 0; i < 11; ++i) {
        for (Size j = 0; j <= i; ++j) {
            if (std::fabs(covariance(v_eu[i][j]) - v_an[i][j]) > tol) {
                BOOST_ERROR("analytical covariance at ("
                            << i << "," << j << ") (" << v_an[i][j] << ") is inconsistent with numerical "
                                                                       "value (Euler discretization, "
                            << covariance(v_eu[i][j]) << "), error is " << v_an[i][j] - covariance(v_eu[i][j])
                            << " tolerance is " << tol);
            }
            if (std::fabs(covariance(v_eu2[i][j]) - v_an[i][j]) > tol) {
                BOOST_ERROR("analytical covariance at ("
                            << i << "," << j << ") (" << v_an[i][j] << ") is inconsistent with numerical "
                                                                       "value (exact discretization, "
                            << covariance(v_eu2[i][j]) << "), error is " << v_an[i][j] - covariance(v_eu2[i][j])
                            << " tolerance is " << tol);
            }
        }
    }

} // testIrFxCrMoments

void CrossAssetModelTest::testIrFxCrCorrelationRecovery() {

    BOOST_TEST_MESSAGE("Test if random correlation input is recovered for "
                       "small dt in ir-fx-cr model...");

    class PseudoCurrency : public Currency {
    public:
        PseudoCurrency(const Size id) {
            std::ostringstream ln, sn;
            ln << "Dummy " << id;
            sn << "DUM " << id;
            data_ = boost::make_shared<Data>(ln.str(), sn.str(), id, sn.str(), "", 100, Rounding(), "%3% %1$.2f");
        }
    };

    Real dt = 1.0E-6;
    Real tol = 1.0E-7;

    // for ir-fx this fully specifies the correlation matrix
    // for new asset classes add other possible combinations as well
    Size currencies[] = { 1, 2, 3, 4, 5, 10, 20 };
    Size creditnames[] = { 0, 1, 5, 10 };

    MersenneTwisterUniformRng mt(42);

    Handle<YieldTermStructure> yts(boost::make_shared<FlatForward>(0, NullCalendar(), 0.01, Actual365Fixed()));

    Handle<DefaultProbabilityTermStructure> hts(
        boost::make_shared<FlatHazardRate>(0, NullCalendar(), 0.01, Actual365Fixed()));

    Handle<Quote> fxspot(boost::make_shared<SimpleQuote>(1.00));

    Array notimes(0);
    Array fxsigma(1, 0.10);

    for (Size ii = 0; ii < LENGTH(currencies); ++ii) {
        for (Size jj = 0; jj < LENGTH(creditnames); ++jj) {

            std::vector<Currency> pseudoCcy;
            for (Size i = 0; i < currencies[ii]; ++i) {
                PseudoCurrency tmp(i);
                pseudoCcy.push_back(tmp);
            }

            Size dim = 2 * currencies[ii] - 1 + creditnames[jj];

            // generate random correlation matrix
            Matrix b(dim, dim);
            Size maxTries = 100;
            bool valid = true;
            do {
                Matrix a(dim, dim);
                for (Size i = 0; i < dim; ++i) {
                    for (Size j = 0; j <= i; ++j) {
                        a[i][j] = a[j][i] = mt.nextReal() - 0.5;
                    }
                }
                b = a * transpose(a);
                for (Size i = 0; i < dim; ++i) {
                    if (b[i][i] < 1E-5)
                        valid = false;
                }
            } while (!valid && --maxTries > 0);

            if (maxTries == 0) {
                BOOST_ERROR("could no generate random matrix");
                return;
            }

            Matrix c(dim, dim);
            for (Size i = 0; i < dim; ++i) {
                for (Size j = 0; j <= i; ++j) {
                    c[i][j] = c[j][i] = b[i][j] / std::sqrt(b[i][i] * b[j][j]);
                }
            }

            // set up model

            std::vector<boost::shared_ptr<Parametrization> > parametrizations;

            // IR
            for (Size i = 0; i < currencies[ii]; ++i) {
                parametrizations.push_back(
                    boost::make_shared<IrLgm1fConstantParametrization>(pseudoCcy[i], yts, 0.01, 0.01));
            }
            // FX
            for (Size i = 0; i < currencies[ii] - 1; ++i) {
                parametrizations.push_back(boost::make_shared<FxBsPiecewiseConstantParametrization>(
                    pseudoCcy[i + 1], fxspot, notimes, fxsigma));
            }
            // CR
            for (Size i = 0; i < creditnames[jj]; ++i) {
                parametrizations.push_back(
                    boost::make_shared<CrLgm1fConstantParametrization>(pseudoCcy[0], hts, 0.01, 0.01));
            }

            boost::shared_ptr<CrossAssetModel> model =
                boost::make_shared<CrossAssetModel>(parametrizations, c, SalvagingAlgorithm::None);

            boost::shared_ptr<StochasticProcess> peuler = model->stateProcess(CrossAssetStateProcess::euler);
            boost::shared_ptr<StochasticProcess> pexact = model->stateProcess(CrossAssetStateProcess::exact);

            Matrix c1 = peuler->covariance(0.0, peuler->initialValues(), dt);
            Matrix c2 = pexact->covariance(0.0, peuler->initialValues(), dt);

            Matrix r1(dim, dim), r2(dim, dim);

            for (Size i = 0; i < dim; ++i) {
                for (Size j = 0; j <= i; ++j) {
                    // there are two state variables per credit name,
                    Size subi = i < 2 * currencies[ii] - 1 ? 1 : 2;
                    Size subj = j < 2 * currencies[ii] - 1 ? 1 : 2;
                    for (Size k1 = 0; k1 < subi; ++k1) {
                        for (Size k2 = 0; k2 < subj; ++k2) {
                            Size i0 = i < 2 * currencies[ii] - 1
                                          ? i
                                          : 2 * currencies[ii] - 1 + 2 * (i - (2 * currencies[ii] - 1)) + k1;
                            Size j0 = j < 2 * currencies[ii] - 1
                                          ? j
                                          : 2 * currencies[ii] - 1 + 2 * (j - (2 * currencies[ii] - 1)) + k2;
                            r1[i][j] = r1[j][i] = c1[i0][j0] / std::sqrt(c1[i0][i0] * c1[j0][j0]);
                            r2[i][j] = r2[j][i] = c2[i0][j0] / std::sqrt(c2[i0][i0] * c2[j0][j0]);
                            if (std::fabs(r1[i][j] - c[i][j]) > tol) {
                                BOOST_ERROR("failed to recover correlation matrix from "
                                            "Euler state process (i,j)=("
                                            << i << "," << j << "), (i0,j0)=(" << i0 << "," << j0
                                            << "), input correlation is " << c[i][j] << ", output is " << r1[i][j]
                                            << ", difference " << (c[i][j] - r1[i][j]) << ", tolerance " << tol
                                            << " test configuration is " << currencies[ii] << " currencies and "
                                            << creditnames[jj] << " credit names");
                            }
                            if (subi == 0 && subj == 0) {
                                if (std::fabs(r2[i][j] - c[i][j]) > tol) {
                                    BOOST_ERROR("failed to recover correlation matrix "
                                                "from "
                                                "exact state process (i,j)=("
                                                << i << "," << j << "), (i0,j0)=(" << i0 << "," << j0
                                                << "), input correlation is " << c[i][j] << ", output is " << r2[i][j]
                                                << ", difference " << (c[i][j] - r2[i][j]) << ", tolerance " << tol
                                                << " test configuration is " << currencies[ii] << " currencies and "
                                                << creditnames[jj] << " credit names");
                                }
                            }
                        }
                    }
                }
            }
        } // for creditnames
    }     // for currenciess

} // testIrFxCrCorrelationRecovery

// =================================================================
// tests for ir-fx-inf-cr
// =================================================================

// this is from the QuantExt test suite (crossassetmodel.cpp),
// with addtional inflation indices, a credit name, and a
// different correlation matrix

struct IrFxInfCrModelTestData {
    IrFxInfCrModelTestData()
        : referenceDate(30, July, 2015), eurYts(boost::make_shared<FlatForward>(referenceDate, 0.02, Actual365Fixed())),
          usdYts(boost::make_shared<FlatForward>(referenceDate, 0.05, Actual365Fixed())),
          gbpYts(boost::make_shared<FlatForward>(referenceDate, 0.04, Actual365Fixed())),
          fxEurUsd(boost::make_shared<SimpleQuote>(0.90)), fxEurGbp(boost::make_shared<SimpleQuote>(1.35)),
          infEurAlpha(0.01), infEurKappa(0.01), infGbpAlpha(0.01), infGbpKappa(0.01),
          n1Ts(boost::make_shared<FlatHazardRate>(referenceDate, 0.01, Actual365Fixed())), n1Alpha(0.01), n1Kappa(0.01),
          c(8, 8, 0.0) {

        std::vector<Date> infDates;
        std::vector<Real> infRates;
        infDates.push_back(Date(30, April, 2015));
        infDates.push_back(Date(30, July, 2015));
        infRates.push_back(0.01);
        infRates.push_back(0.01);
        infEurTs = Handle<ZeroInflationTermStructure>(boost::make_shared<ZeroInflationCurve>(
            referenceDate, TARGET(), Actual365Fixed(), 3 * Months, Monthly, false, eurYts, infDates, infRates));
        infGbpTs = Handle<ZeroInflationTermStructure>(boost::make_shared<ZeroInflationCurve>(
            referenceDate, UnitedKingdom(), Actual365Fixed(), 3 * Months, Monthly, false, eurYts, infDates, infRates));
        infEurTs->enableExtrapolation();
        infGbpTs->enableExtrapolation();
        // same for eur and gbp (doesn't matter anyway, since we are
        // using flat ts here)
        infLag =
            inflationYearFraction(Monthly, false, Actual365Fixed(), infEurTs->baseDate(), infEurTs->referenceDate());

        Settings::instance().evaluationDate() = referenceDate;
        volstepdates.push_back(Date(15, July, 2016));
        volstepdates.push_back(Date(15, July, 2017));
        volstepdates.push_back(Date(15, July, 2018));
        volstepdates.push_back(Date(15, July, 2019));
        volstepdates.push_back(Date(15, July, 2020));

        volstepdatesFx.push_back(Date(15, July, 2016));
        volstepdatesFx.push_back(Date(15, October, 2016));
        volstepdatesFx.push_back(Date(15, May, 2017));
        volstepdatesFx.push_back(Date(13, September, 2017));
        volstepdatesFx.push_back(Date(15, July, 2018));

        volsteptimes_a = Array(volstepdates.size());
        volsteptimesFx_a = Array(volstepdatesFx.size());
        for (Size i = 0; i < volstepdates.size(); ++i) {
            volsteptimes_a[i] = eurYts->timeFromReference(volstepdates[i]);
        }
        for (Size i = 0; i < volstepdatesFx.size(); ++i) {
            volsteptimesFx_a[i] = eurYts->timeFromReference(volstepdatesFx[i]);
        }

        for (Size i = 0; i < volstepdates.size() + 1; ++i) {
            eurVols.push_back(0.0050 + (0.0080 - 0.0050) * std::exp(-0.3 * static_cast<double>(i)));
        }
        for (Size i = 0; i < volstepdates.size() + 1; ++i) {
            usdVols.push_back(0.0030 + (0.0110 - 0.0030) * std::exp(-0.3 * static_cast<double>(i)));
        }
        for (Size i = 0; i < volstepdates.size() + 1; ++i) {
            gbpVols.push_back(0.0070 + (0.0095 - 0.0070) * std::exp(-0.3 * static_cast<double>(i)));
        }
        for (Size i = 0; i < volstepdatesFx.size() + 1; ++i) {
            fxSigmasUsd.push_back(0.15 + (0.20 - 0.15) * std::exp(-0.3 * static_cast<double>(i)));
        }
        for (Size i = 0; i < volstepdatesFx.size() + 1; ++i) {
            fxSigmasGbp.push_back(0.10 + (0.15 - 0.10) * std::exp(-0.3 * static_cast<double>(i)));
        }
        eurVols_a = Array(eurVols.begin(), eurVols.end());
        usdVols_a = Array(usdVols.begin(), usdVols.end());
        gbpVols_a = Array(gbpVols.begin(), gbpVols.end());
        fxSigmasUsd_a = Array(fxSigmasUsd.begin(), fxSigmasUsd.end());
        fxSigmasGbp_a = Array(fxSigmasGbp.begin(), fxSigmasGbp.end());

        notimes_a = Array(0);
        eurKappa_a = Array(1, 0.02);
        usdKappa_a = Array(1, 0.03);
        gbpKappa_a = Array(1, 0.04);

        eurLgm_p = boost::make_shared<IrLgm1fPiecewiseConstantParametrization>(EURCurrency(), eurYts, volsteptimes_a,
                                                                               eurVols_a, notimes_a, eurKappa_a);
        usdLgm_p = boost::make_shared<IrLgm1fPiecewiseConstantParametrization>(USDCurrency(), usdYts, volsteptimes_a,
                                                                               usdVols_a, notimes_a, usdKappa_a);
        gbpLgm_p = boost::make_shared<IrLgm1fPiecewiseConstantParametrization>(GBPCurrency(), gbpYts, volsteptimes_a,
                                                                               gbpVols_a, notimes_a, gbpKappa_a);

        fxUsd_p = boost::make_shared<FxBsPiecewiseConstantParametrization>(USDCurrency(), fxEurUsd, volsteptimesFx_a,
                                                                           fxSigmasUsd_a);
        fxGbp_p = boost::make_shared<FxBsPiecewiseConstantParametrization>(GBPCurrency(), fxEurGbp, volsteptimesFx_a,
                                                                           fxSigmasGbp_a);

        // inflation
        infEur_p = boost::make_shared<InfDkConstantParametrization>(EURCurrency(), infEurTs, infEurAlpha, infEurKappa);
        infGbp_p = boost::make_shared<InfDkConstantParametrization>(GBPCurrency(), infGbpTs, infGbpAlpha, infGbpKappa);

        // credit
        n1_p = boost::make_shared<CrLgm1fConstantParametrization>(EURCurrency(), n1Ts, n1Alpha, n1Kappa);

        singleModels.push_back(eurLgm_p);
        singleModels.push_back(usdLgm_p);
        singleModels.push_back(gbpLgm_p);
        singleModels.push_back(fxUsd_p);
        singleModels.push_back(fxGbp_p);
        singleModels.push_back(infEur_p);
        singleModels.push_back(infGbp_p);
        singleModels.push_back(n1_p);

        Real tmp[8][8] = {
            // EUR  USD GBP  FX1  FX2  CR INF_EUR INF_GBP
            { 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 }, // EUR
            { 0.6, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0 }, // USD
            { 0.3, 0.1, 1.0, 0.0, 0.0, 0.0, 0.0, 0.0 }, // GBP
            { 0.2, 0.2, 0.0, 1.0, 0.0, 0.0, 0.0, 0.0 }, // FX1
            { 0.3, 0.1, 0.1, 0.3, 1.0, 0.0, 0.0, 0.0 }, // FX2
            { 0.8, 0.2, 0.1, 0.4, 0.2, 1.0, 0.0, 0.0 }, // CR
            { 0.6, 0.1, 0.2, 0.2, 0.5, 0.5, 1.0, 0.0 }, // INF_EUR
            { 0.3, 0.2, 0.1, 0.1, 0.3, 0.4, 0.2, 1.0 }  // INF_GBP
        };

        for (Size i = 0; i < 8; ++i) {
            for (Size j = 0; j <= i; ++j) {
                c[i][j] = c[j][i] = tmp[i][j];
            }
        }

        // debug output, to be removed
        // std::clog << "correlation matrix is\n" << c << std::endl;
        // end debug output

        model = boost::make_shared<CrossAssetModel>(singleModels, c, SalvagingAlgorithm::None);
    }

    SavedSettings backup;
    Date referenceDate;
    // ir-fx
    Handle<YieldTermStructure> eurYts, usdYts, gbpYts;
    std::vector<Date> volstepdates, volstepdatesFx;
    Array volsteptimes_a, volsteptimesFx_a;
    std::vector<Real> eurVols, usdVols, gbpVols, fxSigmasUsd, fxSigmasGbp;
    Handle<Quote> fxEurUsd, fxEurGbp;
    Array eurVols_a, usdVols_a, gbpVols_a, fxSigmasUsd_a, fxSigmasGbp_a;
    Array notimes_a, eurKappa_a, usdKappa_a, gbpKappa_a;
    boost::shared_ptr<IrLgm1fParametrization> eurLgm_p, usdLgm_p, gbpLgm_p;
    boost::shared_ptr<FxBsParametrization> fxUsd_p, fxGbp_p;
    // inf
    Handle<ZeroInflationTermStructure> infEurTs, infGbpTs;
    boost::shared_ptr<InfDkParametrization> infEur_p, infGbp_p;
    Real infEurAlpha, infEurKappa, infGbpAlpha, infGbpKappa;
    Real infLag;
    // cr
    Handle<DefaultProbabilityTermStructure> n1Ts, n2Ts, n3Ts;
    boost::shared_ptr<CrLgm1fParametrization> n1_p, n2_p, n3_p;
    Real n1Alpha, n1Kappa, n2Alpha, n2Kappa, n3Alpha, n3Kappa;
    // model
    std::vector<boost::shared_ptr<Parametrization> > singleModels;
    Matrix c;
    boost::shared_ptr<CrossAssetModel> model;
};

void CrossAssetModelTest::testIrFxInfCrMartingaleProperty() {

    BOOST_TEST_MESSAGE("Testing martingale property in ir-fx-inf-cr model for "
                       "Euler and exact discretizations...");

    IrFxInfCrModelTestData d;

    boost::shared_ptr<StochasticProcess> process1 = d.model->stateProcess(CrossAssetStateProcess::exact);
    boost::shared_ptr<StochasticProcess> process2 = d.model->stateProcess(CrossAssetStateProcess::euler);

    Size n = 50000;                         // number of paths
    Size seed = 18;                         // rng seed
    Time T = 10.0;                          // maturity of payoff
    Time T2 = 20.0;                         // zerobond maturity
    Size steps = static_cast<Size>(T * 24); // number of steps taken (euler)

    // this can be made more accurate by using LowDiscrepancy instead
    // of PseudoRandom, but we use an error estimator for the check
    LowDiscrepancy::rsg_type sg1 = LowDiscrepancy::make_sequence_generator(d.model->dimension() * 1, seed);
    LowDiscrepancy::rsg_type sg2 = LowDiscrepancy::make_sequence_generator(d.model->dimension() * steps, seed);

    TimeGrid grid1(T, 1);
    MultiPathGenerator<LowDiscrepancy::rsg_type> pg1(process1, grid1, sg1, false);
    TimeGrid grid2(T, steps);
    MultiPathGenerator<LowDiscrepancy::rsg_type> pg2(process2, grid2, sg2, false);

    accumulator_set<double, stats<tag::mean, tag::error_of<tag::mean> > > eurzb1, usdzb1, gbpzb1, infeur1, infgbp1,
        n1eur1;
    accumulator_set<double, stats<tag::mean, tag::error_of<tag::mean> > > eurzb2, usdzb2, gbpzb2, infeur2, infgbp2,
        n1eur2;

    for (Size j = 0; j < n; ++j) {
        Sample<MultiPath> path1 = pg1.next();
        Sample<MultiPath> path2 = pg2.next();
        Size l1 = path1.value[0].length() - 1;
        Size l2 = path2.value[0].length() - 1;
        Real zeur1 = path1.value[0][l1];
        Real zusd1 = path1.value[1][l1];
        Real zgbp1 = path1.value[2][l1];
        Real fxusd1 = std::exp(path1.value[3][l1]);
        Real fxgbp1 = std::exp(path1.value[4][l1]);
        Real infeurz1 = path1.value[5][l1];
        Real infeury1 = path1.value[6][l1];
        Real infgbpz1 = path1.value[7][l1];
        Real infgbpy1 = path1.value[8][l1];
        Real crzn11 = path1.value[9][l1];
        Real cryn11 = path1.value[10][l1];
        Real zeur2 = path2.value[0][l2];
        Real zusd2 = path2.value[1][l2];
        Real zgbp2 = path2.value[2][l2];
        Real fxusd2 = std::exp(path2.value[3][l2]);
        Real fxgbp2 = std::exp(path2.value[4][l2]);
        Real infeurz2 = path2.value[5][l2];
        Real infeury2 = path2.value[6][l2];
        Real infgbpz2 = path2.value[7][l2];
        Real infgbpy2 = path2.value[8][l2];
        Real crzn12 = path2.value[9][l2];
        Real cryn12 = path2.value[10][l2];

        // EUR zerobond
        eurzb1(d.model->discountBond(0, T, T2, zeur1) / d.model->numeraire(0, T, zeur1));
        // USD zerobond
        usdzb1(d.model->discountBond(1, T, T2, zusd1) * fxusd1 / d.model->numeraire(0, T, zeur1));
        // GBP zerobond
        gbpzb1(d.model->discountBond(2, T, T2, zgbp1) * fxgbp1 / d.model->numeraire(0, T, zeur1));
        // EUR CPI indexed bond
        std::pair<Real, Real> sinfeur1 = d.model->infdkI(0, T, T2, infeurz1, infeury1);
        infeur1(sinfeur1.first * sinfeur1.second * d.model->discountBond(0, T, T2, zeur1) /
                d.model->numeraire(0, T, zeur1));
        // GBP CPI indexed bond
        std::pair<Real, Real> sinfgbp1 = d.model->infdkI(1, T, T2, infgbpz1, infgbpy1);
        infgbp1(sinfgbp1.first * sinfgbp1.second * d.model->discountBond(2, T, T2, zgbp1) * fxgbp1 /
                d.model->numeraire(0, T, zeur1));
        // EUR defaultable zerobond
        std::pair<Real, Real> sn11 = d.model->crlgm1fS(0, 0, T, T2, crzn11, cryn11);
        n1eur1(sn11.first * sn11.second * d.model->discountBond(0, T, T2, zeur1) / d.model->numeraire(0, T, zeur1));

        // EUR zerobond
        eurzb2(d.model->discountBond(0, T, T2, zeur2) / d.model->numeraire(0, T, zeur2));
        // USD zerobond
        usdzb2(d.model->discountBond(1, T, T2, zusd2) * fxusd2 / d.model->numeraire(0, T, zeur2));
        // GBP zerobond
        gbpzb2(d.model->discountBond(2, T, T2, zgbp2) * fxgbp2 / d.model->numeraire(0, T, zeur2));
        // EUR CPI indexed bond
        std::pair<Real, Real> sinfeur2 = d.model->infdkI(0, T, T2, infeurz2, infeury2);
        infeur2(sinfeur2.first * sinfeur2.second * d.model->discountBond(0, T, T2, zeur2) /
                d.model->numeraire(0, T, zeur2));
        // GBP CPI indexed bond
        std::pair<Real, Real> sinfgbp2 = d.model->infdkI(1, T, T2, infgbpz2, infgbpy2);
        infgbp2(sinfgbp2.first * sinfgbp2.second * d.model->discountBond(2, T, T2, zgbp2) * fxgbp2 /
                d.model->numeraire(0, T, zeur2));
        // EUR defaultable zerobond
        std::pair<Real, Real> sn12 = d.model->crlgm1fS(0, 0, T, T2, crzn12, cryn12);
        n1eur2(sn12.first * sn12.second * d.model->discountBond(0, T, T2, zeur2) / d.model->numeraire(0, T, zeur2));
    }

    // debug output, to be removed
    // std::clog << "EXACT:" << std::endl;
    // std::clog << "EUR zb = " << mean(eurzb1) << " +- "
    //           << error_of<tag::mean>(eurzb1) << " vs analytical "
    //           << d.eurYts->discount(T2) << std::endl;
    // std::clog << "USD zb = " << mean(usdzb1) << " +- "
    //           << error_of<tag::mean>(usdzb1) << " vs analytical "
    //           << d.usdYts->discount(T2) * d.fxEurUsd->value() << std::endl;
    // std::clog << "GBP zb = " << mean(gbpzb1) << " +- "
    //           << error_of<tag::mean>(gbpzb1) << " vs analytical "
    //           << d.gbpYts->discount(T2) * d.fxEurGbp->value() << std::endl;
    // std::clog << "IDX zb EUR = " << mean(infeur1) << " +- "
    //           << error_of<tag::mean>(infeur1) << " vs analytical "
    //           << d.eurYts->discount(T2) *
    //                  std::pow(1.0+d.infEurTs->zeroRate(T2 - d.infLag),T2)
    //           << std::endl;
    // std::clog << "IDX zb GBP = " << mean(infgbp1) << " +- "
    //           << error_of<tag::mean>(infgbp1) << " vs analytical "
    //           << d.gbpYts->discount(T2) *
    //                  std::pow(1.0+d.infGbpTs->zeroRate(T2 - d.infLag),T2) *
    //                  d.fxEurGbp->value()
    //           << std::endl;
    // std::clog << "N1 zb EUR = " << mean(n1eur1) << " +- "
    //           << error_of<tag::mean>(n1eur1) << " vs analytical "
    //           << d.eurYts->discount(T2) * d.n1Ts->survivalProbability(T2)
    //           << std::endl;

    // std::clog << "\nEULER:" << std::endl;
    // std::clog << "EUR zb = " << mean(eurzb2) << " +- "
    //           << error_of<tag::mean>(eurzb2) << " vs analytical "
    //           << d.eurYts->discount(T2) << std::endl;
    // std::clog << "USD zb = " << mean(usdzb2) << " +- "
    //           << error_of<tag::mean>(usdzb2) << " vs analytical "
    //           << d.usdYts->discount(T2) * d.fxEurUsd->value() << std::endl;
    // std::clog << "GBP zb = " << mean(gbpzb2) << " +- "
    //           << error_of<tag::mean>(gbpzb2) << " vs analytical "
    //           << d.gbpYts->discount(T2) * d.fxEurGbp->value() << std::endl;
    // std::clog << "IDX zb EUR = " << mean(infeur2) << " +- "
    //           << error_of<tag::mean>(infeur2) << " vs analytical "
    //           << d.eurYts->discount(T2) *
    //                  std::pow(1.0+d.infEurTs->zeroRate(T2 - d.infLag),T2)
    //           << std::endl;
    // std::clog << "IDX zb GBP = " << mean(infgbp2) << " +- "
    //           << error_of<tag::mean>(infgbp2) << " vs analytical "
    //           << d.gbpYts->discount(T2) *
    //                  std::pow(1.0+d.infGbpTs->zeroRate(T2 - d.infLag),T2) *
    //                  d.fxEurGbp->value()
    //           << std::endl;
    // std::clog << "N1 zb EUR = " << mean(n1eur2) << " +- "
    //           << error_of<tag::mean>(n1eur2) << " vs analytical "
    //           << d.eurYts->discount(T2) * d.n1Ts->survivalProbability(T2)
    //           << std::endl;
    // end debug output

    // a bit higher than for plain zero bond , since we look at indexed zero
    // bonds, too
    Real tol1 = 3.0E-4;  // EXACT
    Real tol2 = 14.0E-4; // EULER

    Real ev = d.eurYts->discount(T2);
    if (std::abs(mean(eurzb1) - ev) > tol1)
        BOOST_FAIL("Martingale test failed for eurzb (exact discr.),"
                   "excpected "
                   << ev << ", got " << mean(eurzb1) << ", tolerance " << tol1);
    ev = d.usdYts->discount(T2) * d.fxEurUsd->value();
    if (std::abs(mean(usdzb1) - ev) > tol1)
        BOOST_FAIL("Martingale test failed for eurzb (exact discr.),"
                   "excpected "
                   << ev << ", got " << mean(usdzb1) << ", tolerance " << tol1);
    ev = d.gbpYts->discount(T2) * d.fxEurGbp->value();
    if (std::abs(mean(gbpzb1) - ev) > tol1)
        BOOST_FAIL("Martingale test failed for eurzb (exact discr.),"
                   "excpected "
                   << ev << ", got " << mean(gbpzb1) << ", tolerance " << tol1);
    ev = d.eurYts->discount(T2) * std::pow(1.0 + d.infEurTs->zeroRate(T2 - d.infLag), T2);
    if (std::abs(mean(infeur1) - ev) > tol1)
        BOOST_FAIL("Martingale test failed for idx eurzb (exact discr.),"
                   "excpected "
                   << ev << ", got " << mean(infeur1) << ", tolerance " << tol1);
    ev = d.gbpYts->discount(T2) * std::pow(1.0 + d.infGbpTs->zeroRate(T2 - d.infLag), T2) * d.fxEurGbp->value();
    if (std::abs(mean(infgbp1) - ev) > tol1)
        BOOST_FAIL("Martingale test failed for idx gbpzb (exact discr.),"
                   "excpected "
                   << ev << ", got " << mean(infgbp1) << ", tolerance " << tol1);
    ev = d.eurYts->discount(T2) * d.n1Ts->survivalProbability(T2);
    if (std::abs(mean(n1eur1) - ev) > tol1)
        BOOST_FAIL("Martingale test failed for def eurzb (exact discr.),"
                   "excpected "
                   << ev << ", got " << mean(n1eur1) << ", tolerance " << tol1);

    ev = d.eurYts->discount(T2);
    if (std::abs(mean(eurzb2) - ev) > tol2)
        BOOST_FAIL("Martingale test failed for eurzb (Euler discr.),"
                   "excpected "
                   << ev << ", got " << mean(eurzb2) << ", tolerance " << tol2);
    ev = d.usdYts->discount(T2) * d.fxEurUsd->value();
    if (std::abs(mean(usdzb2) - ev) > tol2)
        BOOST_FAIL("Martingale test failed for usdzb (Euler discr.),"
                   "excpected "
                   << ev << ", got " << mean(usdzb2) << ", tolerance " << tol2 * error_of<tag::mean>(usdzb2));
    ev = d.gbpYts->discount(T2) * d.fxEurGbp->value();
    if (std::abs(mean(gbpzb2) - ev) > tol2)
        BOOST_FAIL("Martingale test failed for gbpzb (Euler discr.),"
                   "excpected "
                   << ev << ", got " << mean(gbpzb2) << ", tolerance " << tol2);
    ev = d.eurYts->discount(T2) * std::pow(1.0 + d.infEurTs->zeroRate(T2 - d.infLag), T2);
    if (std::abs(mean(infeur2) - ev) > tol2)
        BOOST_FAIL("Martingale test failed for idx eurzb (Euler discr.),"
                   "excpected "
                   << ev << ", got " << mean(infeur2) << ", tolerance " << tol1);
    ev = d.gbpYts->discount(T2) * std::pow(1.0 + d.infGbpTs->zeroRate(T2 - d.infLag), T2) * d.fxEurGbp->value();
    if (std::abs(mean(infgbp2) - ev) > tol2)
        BOOST_FAIL("Martingale test failed for idx gbpzb (Euler discr.),"
                   "excpected "
                   << ev << ", got " << mean(infgbp2) << ", tolerance " << tol1);
    ev = d.eurYts->discount(T2) * d.n1Ts->survivalProbability(T2);
    if (std::abs(mean(n1eur2) - ev) > tol2)
        BOOST_FAIL("Martingale test failed for def eurzb (Euler discr.),"
                   "excpected "
                   << ev << ", got " << mean(n1eur1) << ", tolerance " << tol1);

} // testIrFxInfCrMartingaleProperty

void CrossAssetModelTest::testIrFxInfCrMoments() {

    BOOST_TEST_MESSAGE("Testing analytic moments vs. Euler and exact discretization "
                       "in ir-fx-inf-cr model...");

    IrFxInfCrModelTestData d;

    const Size n = 11; // d.model->dimension();

    boost::shared_ptr<StochasticProcess> p_exact = d.model->stateProcess(CrossAssetStateProcess::exact);
    boost::shared_ptr<StochasticProcess> p_euler = d.model->stateProcess(CrossAssetStateProcess::euler);

    Real T = 10;                            // horizon at which we compare the moments
    Size steps = static_cast<Size>(T * 10); // number of simulation steps (Euler and exact)
    Size paths = 30000;                     // number of paths

    Array e_an = p_exact->expectation(0.0, p_exact->initialValues(), T);
    Matrix v_an = p_exact->covariance(0.0, p_exact->initialValues(), T);

    Size seed = 18;
    TimeGrid grid(T, steps);

    MultiPathGeneratorSobolBrownianBridge pgen(p_euler, grid, SobolBrownianGenerator::Diagonal, seed,
                                               SobolRsg::JoeKuoD7);
    MultiPathGeneratorSobolBrownianBridge pgen2(p_exact, grid, SobolBrownianGenerator::Diagonal, seed,
                                                SobolRsg::JoeKuoD7);

    accumulator_set<double, stats<tag::mean, tag::error_of<tag::mean> > > e_eu[n], e_eu2[n];
    accumulator_set<double, stats<tag::covariance<double, tag::covariate1> > > v_eu[n][n], v_eu2[n][n];

    for (Size i = 0; i < paths; ++i) {
        Sample<MultiPath> path = pgen.next();
        Sample<MultiPath> path2 = pgen2.next();
        for (Size ii = 0; ii < n; ++ii) {
            Real cii = path.value[ii].back();
            Real cii2 = path2.value[ii].back();
            e_eu[ii](cii);
            e_eu2[ii](cii2);
            for (Size jj = 0; jj <= ii; ++jj) {
                Real cjj = path.value[jj].back();
                v_eu[ii][jj](cii, covariate1 = cjj);
                Real cjj2 = path2.value[jj].back();
                v_eu2[ii][jj](cii2, covariate1 = cjj2);
            }
        }
    }

    // debug, to be removed
    // for (Size i = 0; i < n; ++i) {
    //     std::clog << "E_" << i << " " << e_an[i] << " " << mean(e_eu[i]) << "
    //     "
    //               << mean(e_eu2[i]) << std::endl;
    // }
    // std::clog << "==================" << std::endl;

    // std::clog << "one step analytical" << std::endl;
    // for (Size i = 0; i < n; ++i) {
    //     for (Size j = 0; j <= i; ++j) {
    //         std::clog << v_an[i][j] << " ";
    //     }
    //     std::clog << std::endl;
    // }
    // std::clog << "==================" << std::endl;

    // std::clog << "euler" << std::endl;
    // for (Size i = 0; i < n; ++i) {
    //     for (Size j = 0; j <= i; ++j) {
    //         std::clog << covariance(v_eu[i][j]) << " ";
    //     }
    //     std::clog << std::endl;
    // }
    // std::clog << "==================" << std::endl;

    // std::clog << "exact" << std::endl;
    // for (Size i = 0; i < n; ++i) {
    //     for (Size j = 0; j <= i; ++j) {
    //         std::clog << covariance(v_eu2[i][j]) << " ";
    //     }
    //     std::clog << std::endl;
    // }
    // std::clog << "==================" << std::endl;

    // end debug

    Real errTolLd[] = { 0.5E-4, 0.5E-4, 0.5E-4, 10.0E-4, 10.0E-4, 0.7E-4, 0.7E-4, 0.7E-4, 0.7E-4, 0.7E-4, 0.7E-4 };

    for (Size i = 0; i < n; ++i) {
        // check expectation against analytical calculation (Euler)
        if (std::fabs(mean(e_eu[i]) - e_an[i]) > errTolLd[i]) {
            BOOST_ERROR("analytical expectation for component #"
                        << i << " (" << e_an[i] << ") is inconsistent with numerical value (Euler "
                                                   "discretization, "
                        << mean(e_eu[i]) << "), error is " << e_an[i] - mean(e_eu[i]) << " tolerance is "
                        << errTolLd[i]);
        }
        // check expectation against analytical calculation (exact disc)
        if (std::fabs(mean(e_eu2[i]) - e_an[i]) > errTolLd[i]) {
            BOOST_ERROR("analytical expectation for component #"
                        << i << " (" << e_an[i] << ") is inconsistent with numerical value (exact "
                                                   "discretization, "
                        << mean(e_eu2[i]) << "), error is " << e_an[i] - mean(e_eu2[i]) << " tolerance is "
                        << errTolLd[i]);
        }
    }

    // as above, this is a bit rough compared to the more differentiated
    // test of the IR-FX model ...
    Real tol = 10.0E-4;

    for (Size i = 0; i < n; ++i) {
        for (Size j = 0; j <= i; ++j) {
            if (std::fabs(covariance(v_eu[i][j]) - v_an[i][j]) > tol) {
                BOOST_ERROR("analytical covariance at ("
                            << i << "," << j << ") (" << v_an[i][j] << ") is inconsistent with numerical "
                                                                       "value (Euler discretization, "
                            << covariance(v_eu[i][j]) << "), error is " << v_an[i][j] - covariance(v_eu[i][j])
                            << " tolerance is " << tol);
            }
            if (std::fabs(covariance(v_eu2[i][j]) - v_an[i][j]) > tol) {
                BOOST_ERROR("analytical covariance at ("
                            << i << "," << j << ") (" << v_an[i][j] << ") is inconsistent with numerical "
                                                                       "value (exact discretization, "
                            << covariance(v_eu2[i][j]) << "), error is " << v_an[i][j] - covariance(v_eu2[i][j])
                            << " tolerance is " << tol);
            }
        }
    }

} // testIrFxInfCrMoments

void CrossAssetModelTest::testCorrelationRecovery() {

    BOOST_TEST_MESSAGE("Test if random correlation input is recovered for "
                       "small dt in Ccy LGM model...");

    class PseudoCurrency : public Currency {
    public:
        PseudoCurrency(const Size id) {
            std::ostringstream ln, sn;
            ln << "Dummy " << id;
            sn << "DUM " << id;
            data_ = boost::make_shared<Data>(ln.str(), sn.str(), id, sn.str(), "", 100, Rounding(), "%3% %1$.2f");
        }
    };

    Real dt = 1.0E-6;
    Real tol = 1.0E-7;

    // for ir-fx this fully specifies the correlation matrix
    // for new asset classes add other possible combinations as well
    Size currencies[] = { 1, 2, 3, 4, 5, 10, 20, 50, 100 };

    MersenneTwisterUniformRng mt(42);

    Handle<YieldTermStructure> yts(boost::make_shared<FlatForward>(0, NullCalendar(), 0.01, Actual365Fixed()));

    Handle<Quote> fxspot(boost::make_shared<SimpleQuote>(1.00));

    Array notimes(0);
    Array fxsigma(1, 0.10);

    for (Size ii = 0; ii < LENGTH(currencies); ++ii) {

        std::vector<Currency> pseudoCcy;
        for (Size i = 0; i < currencies[ii]; ++i) {
            PseudoCurrency tmp(i);
            pseudoCcy.push_back(tmp);
        }

        Size dim = 2 * currencies[ii] - 1;

        // generate random correlation matrix
        Matrix b(dim, dim);
        Size maxTries = 100;
        bool valid = true;
        do {
            Matrix a(dim, dim);
            for (Size i = 0; i < dim; ++i) {
                for (Size j = 0; j <= i; ++j) {
                    a[i][j] = a[j][i] = mt.nextReal() - 0.5;
                }
            }
            b = a * transpose(a);
            for (Size i = 0; i < dim; ++i) {
                if (b[i][i] < 1E-5)
                    valid = false;
            }
        } while (!valid && --maxTries > 0);

        if (maxTries == 0) {
            BOOST_ERROR("could no generate random matrix");
            return;
        }

        Matrix c(dim, dim);
        for (Size i = 0; i < dim; ++i) {
            for (Size j = 0; j <= i; ++j) {
                c[i][j] = c[j][i] = b[i][j] / std::sqrt(b[i][i] * b[j][j]);
            }
        }

        // set up model

        std::vector<boost::shared_ptr<Parametrization> > parametrizations;

        // IR
        for (Size i = 0; i < currencies[ii]; ++i) {
            parametrizations.push_back(
                boost::make_shared<IrLgm1fConstantParametrization>(pseudoCcy[i], yts, 0.01, 0.01));
        }
        // FX
        for (Size i = 0; i < currencies[ii] - 1; ++i) {
            parametrizations.push_back(
                boost::make_shared<FxBsPiecewiseConstantParametrization>(pseudoCcy[i + 1], fxspot, notimes, fxsigma));
        }

        boost::shared_ptr<CrossAssetModel> model =
            boost::make_shared<CrossAssetModel>(parametrizations, c, SalvagingAlgorithm::None);

        boost::shared_ptr<StochasticProcess> peuler = model->stateProcess(CrossAssetStateProcess::euler);
        boost::shared_ptr<StochasticProcess> pexact = model->stateProcess(CrossAssetStateProcess::exact);

        Matrix c1 = peuler->covariance(0.0, peuler->initialValues(), dt);
        Matrix c2 = pexact->covariance(0.0, peuler->initialValues(), dt);

        Matrix r1(dim, dim), r2(dim, dim);

        for (Size i = 0; i < dim; ++i) {
            for (Size j = 0; j <= i; ++j) {
                r1[i][j] = r1[j][i] = c1[i][j] / std::sqrt(c1[i][i] * c1[j][j]);
                r2[i][j] = r2[j][i] = c2[i][j] / std::sqrt(c2[i][i] * c2[j][j]);
                if (std::fabs(r1[i][j] - c[i][j]) > tol) {
                    BOOST_ERROR("failed to recover correlation matrix from "
                                "Euler state process (i,j)=("
                                << i << "," << j << "), input correlation is " << c[i][j] << ", output is " << r1[i][j]
                                << ", difference " << (c[i][j] - r1[i][j]) << ", tolerance " << tol);
                }
                if (std::fabs(r2[i][j] - c[i][j]) > tol) {
                    BOOST_ERROR("failed to recover correlation matrix from "
                                "exact state process (i,j)=("
                                << i << "," << j << "), input correlation is " << c[i][j] << ", output is " << r2[i][j]
                                << ", difference " << (c[i][j] - r2[i][j]) << ", tolerance " << tol);
                }
            }
        }

    } // for currencies

} // test correlation recovery

void CrossAssetModelTest::testIrFxInfCrCorrelationRecovery() {

    BOOST_TEST_MESSAGE("Test if random correlation input is recovered for "
                       "small dt in ir-fx-inf-cr model...");

    SavedSettings backup;
    Settings::instance().evaluationDate() = Date(30, July, 2015);

    class PseudoCurrency : public Currency {
    public:
        PseudoCurrency(const Size id) {
            std::ostringstream ln, sn;
            ln << "Dummy " << id;
            sn << "DUM " << id;
            data_ = boost::make_shared<Data>(ln.str(), sn.str(), id, sn.str(), "", 100, Rounding(), "%3% %1$.2f");
        }
    };

    Real dt = 1.0E-6;
    Real tol = 1.0E-7;

    // for ir-fx this fully specifies the correlation matrix
    // for new asset classes add other possible combinations as well
    Size currencies[] = { 1, 2, 3, 4, 5, 10, 20 };
    Size cpiindexes[] = { 0, 1, 10 };
    Size creditnames[] = { 0, 1, 5 };

    MersenneTwisterUniformRng mt(42);

    Handle<YieldTermStructure> yts(boost::make_shared<FlatForward>(0, NullCalendar(), 0.01, Actual365Fixed()));

    std::vector<Date> infDates;
    std::vector<Real> infRates;
    infDates.push_back(Date(30, April, 2015));
    infDates.push_back(Date(30, July, 2015));
    infRates.push_back(0.01);
    infRates.push_back(0.01);
    Handle<ZeroInflationTermStructure> its(
        boost::make_shared<ZeroInflationCurve>(Settings::instance().evaluationDate(), NullCalendar(), Actual365Fixed(),
                                               3 * Months, Monthly, false, yts, infDates, infRates));

    Handle<DefaultProbabilityTermStructure> hts(
        boost::make_shared<FlatHazardRate>(0, NullCalendar(), 0.01, Actual365Fixed()));

    Handle<Quote> fxspot(boost::make_shared<SimpleQuote>(1.00));

    Array notimes(0);
    Array fxsigma(1, 0.10);

    for (Size ii = 0; ii < LENGTH(currencies); ++ii) {
        for (Size kk = 0; kk < LENGTH(cpiindexes); ++kk) {
            for (Size jj = 0; jj < LENGTH(creditnames); ++jj) {

                std::vector<Currency> pseudoCcy;
                for (Size i = 0; i < currencies[ii]; ++i) {
                    PseudoCurrency tmp(i);
                    pseudoCcy.push_back(tmp);
                }

                Size dim = 2 * currencies[ii] - 1 + cpiindexes[kk] + creditnames[jj];

                // generate random correlation matrix
                Matrix b(dim, dim);
                Size maxTries = 100;
                bool valid = true;
                do {
                    Matrix a(dim, dim);
                    for (Size i = 0; i < dim; ++i) {
                        for (Size j = 0; j <= i; ++j) {
                            a[i][j] = a[j][i] = mt.nextReal() - 0.5;
                        }
                    }
                    b = a * transpose(a);
                    for (Size i = 0; i < dim; ++i) {
                        if (b[i][i] < 1E-5)
                            valid = false;
                    }
                } while (!valid && --maxTries > 0);

                if (maxTries == 0) {
                    BOOST_ERROR("could no generate random matrix");
                    return;
                }

                Matrix c(dim, dim);
                for (Size i = 0; i < dim; ++i) {
                    for (Size j = 0; j <= i; ++j) {
                        c[i][j] = c[j][i] = b[i][j] / std::sqrt(b[i][i] * b[j][j]);
                    }
                }

                // set up model

                std::vector<boost::shared_ptr<Parametrization> > parametrizations;

                // IR
                for (Size i = 0; i < currencies[ii]; ++i) {
                    parametrizations.push_back(
                        boost::make_shared<IrLgm1fConstantParametrization>(pseudoCcy[i], yts, 0.01, 0.01));
                }
                // FX
                for (Size i = 0; i < currencies[ii] - 1; ++i) {
                    parametrizations.push_back(boost::make_shared<FxBsPiecewiseConstantParametrization>(
                        pseudoCcy[i + 1], fxspot, notimes, fxsigma));
                }
                // INF
                for (Size i = 0; i < cpiindexes[kk]; ++i) {
                    parametrizations.push_back(
                        boost::make_shared<InfDkConstantParametrization>(pseudoCcy[0], its, 0.01, 0.01));
                }
                // CR
                for (Size i = 0; i < creditnames[jj]; ++i) {
                    parametrizations.push_back(
                        boost::make_shared<CrLgm1fConstantParametrization>(pseudoCcy[0], hts, 0.01, 0.01));
                }

                boost::shared_ptr<CrossAssetModel> model =
                    boost::make_shared<CrossAssetModel>(parametrizations, c, SalvagingAlgorithm::None);

                boost::shared_ptr<StochasticProcess> peuler = model->stateProcess(CrossAssetStateProcess::euler);
                boost::shared_ptr<StochasticProcess> pexact = model->stateProcess(CrossAssetStateProcess::exact);

                Matrix c1 = peuler->covariance(0.0, peuler->initialValues(), dt);
                Matrix c2 = pexact->covariance(0.0, peuler->initialValues(), dt);

                Matrix r1(dim, dim), r2(dim, dim);

                for (Size i = 0; i < dim; ++i) {
                    for (Size j = 0; j <= i; ++j) {
                        // there are two state variables per credit name,
                        // and per inflation index
                        Size subi = i < 2 * currencies[ii] - 1 ? 1 : 2;
                        Size subj = j < 2 * currencies[ii] - 1 ? 1 : 2;
                        for (Size k1 = 0; k1 < subi; ++k1) {
                            for (Size k2 = 0; k2 < subj; ++k2) {
                                Size i0 = i < 2 * currencies[ii] - 1
                                              ? i
                                              : 2 * currencies[ii] - 1 + 2 * (i - (2 * currencies[ii] - 1)) + k1;
                                Size j0 = j < 2 * currencies[ii] - 1
                                              ? j
                                              : 2 * currencies[ii] - 1 + 2 * (j - (2 * currencies[ii] - 1)) + k2;
                                r1[i][j] = r1[j][i] = c1[i0][j0] / std::sqrt(c1[i0][i0] * c1[j0][j0]);
                                r2[i][j] = r2[j][i] = c2[i0][j0] / std::sqrt(c2[i0][i0] * c2[j0][j0]);
                                if (std::fabs(r1[i][j] - c[i][j]) > tol) {
                                    BOOST_ERROR("failed to recover correlation matrix "
                                                "from "
                                                "Euler state process (i,j)=("
                                                << i << "," << j << "), (i0,j0)=(" << i0 << "," << j0
                                                << "), input correlation is " << c[i][j] << ", output is " << r1[i][j]
                                                << ", difference " << (c[i][j] - r1[i][j]) << ", tolerance " << tol
                                                << " test configuration is " << currencies[ii] << " currencies and "
                                                << cpiindexes[kk] << " cpi indexes and " << creditnames[jj]
                                                << " credit names");
                                }
                                if (subi == 0 && subj == 0) {
                                    if (std::fabs(r2[i][j] - c[i][j]) > tol) {
                                        BOOST_ERROR("failed to recover correlation "
                                                    "matrix "
                                                    "from "
                                                    "exact state process (i,j)=("
                                                    << i << "," << j << "), (i0,j0)=(" << i0 << "," << j0
                                                    << "), input correlation is " << c[i][j] << ", output is "
                                                    << r2[i][j] << ", difference " << (c[i][j] - r2[i][j])
                                                    << ", tolerance " << tol << " test configuration is "
                                                    << currencies[ii] << " currencies and " << cpiindexes[kk]
                                                    << " cpi indexes and " << creditnames[jj] << " credit names");
                                    }
                                }
                            }
                        }
                    }
                }
            } // for creditnames
        }     // for cpiindexes
    }         // for currenciess
} // testIrFxInfCrCorrelationRecovery

void CrossAssetModelTest::testCpiCalibrationByAlpha() {

    BOOST_TEST_MESSAGE("Testing calibration to ZC CPI Floors (using alpha) and repricing via MC...");

    // set up IR-INF model, calibrate to given premiums and check
    // the result with a MC simulation

    SavedSettings backup;
    Date refDate(30, July, 2015);
    Settings::instance().evaluationDate() = Date(30, July, 2015);

    // IR
    Handle<YieldTermStructure> eurYts(boost::make_shared<FlatForward>(refDate, 0.01, Actual365Fixed()));
    boost::shared_ptr<Parametrization> ireur_p =
        boost::make_shared<IrLgm1fConstantParametrization>(EURCurrency(), eurYts, 0.01, 0.01);

    // INF
    Real baseCPI = 100.0;
    std::vector<Date> infDates;
    std::vector<Real> infRates;
    infDates.push_back(Date(30, April, 2015));
    infDates.push_back(Date(30, July, 2015));
    infRates.push_back(0.0075);
    infRates.push_back(0.0075);
    Handle<ZeroInflationTermStructure> infEurTs(boost::make_shared<ZeroInflationCurve>(
        refDate, TARGET(), Actual365Fixed(), 3 * Months, Monthly, false, eurYts, infDates, infRates));
    infEurTs->enableExtrapolation();
    Handle<ZeroInflationIndex> infIndex(boost::make_shared<EUHICPXT>(false, infEurTs));

    Real premium[] = { 0.0044, 0.0085, 0.0127, 0.0160, 0.0186 };

    std::vector<boost::shared_ptr<CalibrationHelper> > cpiHelpers;
    Array volStepTimes(4), noTimes(0);
    Array infVols(5, 0.01), infRev(1, 1.5); // !!

    Time T;
    for (Size i = 1; i <= 5; ++i) {
        Date maturity = refDate + i * Years;
        boost::shared_ptr<CpiCapFloorHelper> h(new CpiCapFloorHelper(Option::Put, baseCPI, maturity, TARGET(),
                                                                     ModifiedFollowing, TARGET(), ModifiedFollowing,
                                                                     0.01, infIndex, 3 * Months, premium[i - 1]));
        Real t = inflationYearFraction(Monthly, false, Actual365Fixed(), infEurTs->baseDate(),
                                       h->instrument()->fixingDate());
        cpiHelpers.push_back(h);
        if (i <= 4)
            volStepTimes[i - 1] = t;
        T = t;
    }

    boost::shared_ptr<InfDkPiecewiseConstantParametrization> infeur_p =
        boost::make_shared<InfDkPiecewiseConstantParametrization>(EURCurrency(), infEurTs, volStepTimes, infVols,
                                                                  noTimes, infRev);

    std::vector<boost::shared_ptr<Parametrization> > parametrizations;
    parametrizations.push_back(ireur_p);
    parametrizations.push_back(infeur_p);

    boost::shared_ptr<CrossAssetModel> model =
        boost::make_shared<CrossAssetModel>(parametrizations, Matrix(), SalvagingAlgorithm::None);

    model->correlation(IR, 0, INF, 0, 0.33);

    // pricing engine
    boost::shared_ptr<AnalyticDkCpiCapFloorEngine> engine =
        boost::make_shared<AnalyticDkCpiCapFloorEngine>(model, 0, baseCPI);

    for (Size i = 0; i < cpiHelpers.size(); ++i) {
        cpiHelpers[i]->setPricingEngine(engine);
    }

    // calibration
    LevenbergMarquardt lm;
    EndCriteria ec(1000, 500, 1E-8, 1E-8, 1E-8);
    model->calibrateInfDkVolatilitiesIterative(0, cpiHelpers, lm, ec);

    // debug output (can be removed again)
    // for (Size i = 0; i < cpiHelpers.size(); ++i) {
    //     BOOST_TEST_MESSAGE("i=" << i << " modelvol="
    //                             << model->infdk(0)->parameterValues(0)[i]
    //                             << " market=" << cpiHelpers[i]->marketValue()
    //                             << " model=" << cpiHelpers[i]->modelValue()
    //                             << " diff=" << (cpiHelpers[i]->marketValue() -
    //                                             cpiHelpers[i]->modelValue()));
    // }
    // end debug output

    // reprice last ZC floor with Monte Carlo
    Size n = 50000; // number of paths
    Size seed = 18; // rng seed
    Size steps = 1; // number of discretization steps

    boost::shared_ptr<StochasticProcess> process = model->stateProcess(CrossAssetStateProcess::exact);
    LowDiscrepancy::rsg_type sg = LowDiscrepancy::make_sequence_generator(model->dimension() * steps, seed);
    TimeGrid grid(T, steps);
    MultiPathGenerator<LowDiscrepancy::rsg_type> pg(process, grid, sg, false);

    accumulator_set<double, stats<tag::mean, tag::error_of<tag::mean> > > floor;

    Real K = std::pow(1.0 + 0.01, T);

    for (Size i = 0; i < n; ++i) {
        Sample<MultiPath> path = pg.next();
        Size l = path.value[0].length() - 1;
        Real irz = path.value[0][l];
        Real infz = path.value[1][l];
        Real infy = path.value[2][l];
        Real I = model->infdkI(0, T, T, infz, infy).first;
        floor(std::max(-(I - K), 0.0) / model->numeraire(0, T, irz));
    }

    // debug output
    // BOOST_TEST_MESSAGE("mc floor 5y = " << mean(floor) << " +- "
    //                                     << error_of<tag::mean>(floor));
    // end debug output

    // check model calibration
    Real tol = 1.0E-12;
    for (Size i = 0; i < cpiHelpers.size(); ++i) {
        if (std::abs(cpiHelpers[i]->modelValue() - cpiHelpers[i]->marketValue()) > tol) {
            BOOST_ERROR("Model calibration for ZC CPI Floor #"
                        << i << " failed, market premium is " << cpiHelpers[i]->marketValue() << ", model value is "
                        << cpiHelpers[i]->modelValue() << ", difference is "
                        << (cpiHelpers[i]->marketValue() - cpiHelpers[i]->modelValue()) << ", tolerance is " << tol);
        }
    }
    // check repricing with MC
    tol = 1.0E-5;
    Real mcPrice = mean(floor);
    if (std::abs(mcPrice - cpiHelpers[4]->modelValue()) > tol) {
        BOOST_ERROR("Failed to reprice 5y ZC CPI Floor with MC ("
                    << mcPrice << "), analytical model price is " << cpiHelpers[4]->modelValue() << ", difference is "
                    << mcPrice - cpiHelpers[4]->modelValue() << ", tolerance is " << tol);
    }
}

void CrossAssetModelTest::testCpiCalibrationByH() {

    BOOST_TEST_MESSAGE("Testing calibration to ZC CPI Floors (using H) and repricing via MC...");

    // set up IR-INF model, calibrate to given premiums and check
    // the result with a MC simulation

    SavedSettings backup;
    Date refDate(30, July, 2015);
    Settings::instance().evaluationDate() = Date(30, July, 2015);

    // IR
    Handle<YieldTermStructure> eurYts(boost::make_shared<FlatForward>(refDate, 0.01, Actual365Fixed()));
    boost::shared_ptr<Parametrization> ireur_p =
        boost::make_shared<IrLgm1fConstantParametrization>(EURCurrency(), eurYts, 0.01, 0.01);

    // INF
    Real baseCPI = 100.0;
    std::vector<Date> infDates;
    std::vector<Real> infRates;
    infDates.push_back(Date(30, April, 2015));
    infDates.push_back(Date(30, July, 2015));
    infRates.push_back(0.0075);
    infRates.push_back(0.0075);
    Handle<ZeroInflationTermStructure> infEurTs(boost::make_shared<ZeroInflationCurve>(
        refDate, TARGET(), Actual365Fixed(), 3 * Months, Monthly, false, eurYts, infDates, infRates));
    infEurTs->enableExtrapolation();
    Handle<ZeroInflationIndex> infIndex(boost::make_shared<EUHICPXT>(false, infEurTs));

    Size nMat = 14;
    Real premium[] = { 0.000555, 0.000813, 0.000928, 0.00127, 0.001616, 0.0019, 0.0023,
                       0.0026,   0.0029,   0.0032,   0.0032,  0.0033,   0.0038, 0.0067 };
    Period maturity[] = { 1 * Years, 2 * Years, 3 * Years,  4 * Years,  5 * Years,  6 * Years,  7 * Years,
                          8 * Years, 9 * Years, 10 * Years, 12 * Years, 15 * Years, 20 * Years, 30 * Years };

    std::vector<boost::shared_ptr<CalibrationHelper> > cpiHelpers;
    Array volStepTimes(13), noTimes(0);
    Array infVols(14, 0.0030), infRev(14, 1.0); // init vol and rev !!
    Real strike = 0.00;                         // strike !!

    Time T;
    for (Size i = 1; i <= nMat; ++i) {
        Date mat = refDate + maturity[i - 1];
        boost::shared_ptr<CpiCapFloorHelper> h(new CpiCapFloorHelper(Option::Put, baseCPI, mat, TARGET(),
                                                                     ModifiedFollowing, TARGET(), ModifiedFollowing,
                                                                     strike, infIndex, 3 * Months, premium[i - 1]));
        Real t = inflationYearFraction(Monthly, false, Actual365Fixed(), infEurTs->baseDate(),
                                       h->instrument()->fixingDate());
        cpiHelpers.push_back(h);
        if (i <= nMat - 1)
            volStepTimes[i - 1] = t;
        T = t;
    }

    boost::shared_ptr<InfDkPiecewiseLinearParametrization> infeur_p =
        boost::make_shared<InfDkPiecewiseLinearParametrization>(EURCurrency(), infEurTs, volStepTimes, infVols,
                                                                volStepTimes, infRev);

    std::vector<boost::shared_ptr<Parametrization> > parametrizations;
    parametrizations.push_back(ireur_p);
    parametrizations.push_back(infeur_p);

    boost::shared_ptr<CrossAssetModel> model =
        boost::make_shared<CrossAssetModel>(parametrizations, Matrix(), SalvagingAlgorithm::None);

    model->correlation(IR, 0, INF, 0, 0.33);

    // pricing engine
    boost::shared_ptr<AnalyticDkCpiCapFloorEngine> engine =
        boost::make_shared<AnalyticDkCpiCapFloorEngine>(model, 0, baseCPI);

    for (Size i = 0; i < cpiHelpers.size(); ++i) {
        cpiHelpers[i]->setPricingEngine(engine);
    }

    // calibration
    LevenbergMarquardt lm;
    EndCriteria ec(1000, 500, 1E-8, 1E-8, 1E-8);
    // model->calibrateInfDkVolatilitiesIterative(0, cpiHelpers, lm, ec);
    model->calibrateInfDkReversionsIterative(0, cpiHelpers, lm, ec);

    // debug output (can be removed again)
    // for (Size i = 0; i < cpiHelpers.size(); ++i) {
    //     BOOST_TEST_MESSAGE("i=" << i << " modelvol=" << model->infdk(0)->parameterValues(0)[i] << " modelrev="
    //                             << model->infdk(0)->parameterValues(1)[i] << " market=" <<
    //                             cpiHelpers[i]->marketValue()
    //                             << " model=" << cpiHelpers[i]->modelValue()
    //                             << " diff=" << (cpiHelpers[i]->marketValue() - cpiHelpers[i]->modelValue()));
    // }
    // end debug output

    // reprice last ZC floor with Monte Carlo
    Size n = 100000; // number of paths
    Size seed = 18;  // rng seed
    Size steps = 1;  // number of discretization steps

    boost::shared_ptr<StochasticProcess> process = model->stateProcess(CrossAssetStateProcess::exact);
    LowDiscrepancy::rsg_type sg = LowDiscrepancy::make_sequence_generator(model->dimension() * steps, seed);
    TimeGrid grid(T, steps);
    MultiPathGenerator<LowDiscrepancy::rsg_type> pg(process, grid, sg, false);

    accumulator_set<double, stats<tag::mean, tag::error_of<tag::mean> > > floor;

    Real K = std::pow(1.0 + strike, T);

    for (Size i = 0; i < n; ++i) {
        Sample<MultiPath> path = pg.next();
        Size l = path.value[0].length() - 1;
        Real irz = path.value[0][l];
        Real infz = path.value[1][l];
        Real infy = path.value[2][l];
        Real I = model->infdkI(0, T, T, infz, infy).first;
        floor(std::max(-(I - K), 0.0) / model->numeraire(0, T, irz));
    }

    // debug output
    // BOOST_TEST_MESSAGE("mc floor last = " << mean(floor) << " +- " << error_of<tag::mean>(floor));
    // end debug output

    // check model calibration
    Real tol = 1.0E-12;
    for (Size i = 0; i < cpiHelpers.size(); ++i) {
        if (std::abs(cpiHelpers[i]->modelValue() - cpiHelpers[i]->marketValue()) > tol) {
            BOOST_ERROR("Model calibration for ZC CPI Floor #"
                        << i << " failed, market premium is " << cpiHelpers[i]->marketValue() << ", model value is "
                        << cpiHelpers[i]->modelValue() << ", difference is "
                        << (cpiHelpers[i]->marketValue() - cpiHelpers[i]->modelValue()) << ", tolerance is " << tol);
        }
    }
    // check repricing with MC
    tol = 2.0E-4;
    Real mcPrice = mean(floor);
    if (std::abs(mcPrice - cpiHelpers[nMat - 1]->modelValue()) > tol) {
        BOOST_ERROR("Failed to reprice last ZC CPI Floor with MC ("
                    << mcPrice << "), analytical model price is " << cpiHelpers[4]->modelValue() << ", difference is "
                    << mcPrice - cpiHelpers[nMat - 1]->modelValue() << ", tolerance is " << tol);
    }
}

test_suite* CrossAssetModelTest::suite() {
    test_suite* suite = BOOST_TEST_SUITE("CrossAsset model tests");
    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testBermudanLgm1fGsr));
    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testBermudanLgmInvariances));
    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testNonstandardBermudanSwaption));

    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testLgm1fCalibration));
    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testCcyLgm3fForeignPayouts));

    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testLgm5fFxCalibration));
    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testLgm5fFullCalibration));
    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testLgm5fMoments));

    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testLgmGsrEquivalence));
    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testLgmMcWithShift));

    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testIrFxCrMartingaleProperty));
    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testIrFxCrMoments));
    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testIrFxInfCrMartingaleProperty));
    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testIrFxInfCrMoments));

    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testCpiCalibrationByAlpha));
    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testCpiCalibrationByH));

    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testCorrelationRecovery));
    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testIrFxCrCorrelationRecovery));
    suite->add(QUANTLIB_TEST_CASE(&CrossAssetModelTest::testIrFxInfCrCorrelationRecovery));
    
    return suite;
}
}
