/*
 Copyright (C) 2021 Quaternion Risk Management Ltd
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

#include <ored/portfolio/bondposition.hpp>
#include <ored/portfolio/equityoptionposition.hpp>
#include <ored/portfolio/equityposition.hpp>
#include <ored/portfolio/trsunderlyingbuilder.hpp>
#include <qle/indexes/compositeindex.hpp>

#include <ored/portfolio/bond.hpp>
#include <ored/portfolio/forwardbond.hpp>
#include <ored/utilities/marketdata.hpp>

#include <qle/instruments/forwardbond.hpp>

namespace ore {
namespace data {

boost::shared_ptr<TrsUnderlyingBuilder> TrsUnderlyingBuilderFactory::getBuilder(const std::string& tradeType) const {
    boost::shared_lock<boost::shared_mutex> lock(mutex_);
    auto b = builders_.find(tradeType);
    QL_REQUIRE(b != builders_.end(), "TrsUnderlyingBuilderFactory::getBuilder(" << tradeType << "): no builder found");
    return b->second;
}

void TrsUnderlyingBuilderFactory::addBuilder(const std::string& tradeType,
                                             const boost::shared_ptr<TrsUnderlyingBuilder>& builder,
                                             const bool allowOverwrite) {
    boost::unique_lock<boost::shared_mutex> locK(mutex_);
    QL_REQUIRE(builders_.insert(std::make_pair(tradeType, builder)).second || allowOverwrite,
               "TrsUnderlyingBuidlerFactory::addBuilder(" << tradeType << "): builder for key already exists.");
}

void BondTrsUnderlyingBuilder::build(
    const std::string& parentId, const boost::shared_ptr<Trade>& underlying, const std::vector<Date>& valuationDates,
    const boost::shared_ptr<EngineFactory>& engineFactory, boost::shared_ptr<QuantLib::Index>& underlyingIndex,
    Real& underlyingMultiplier, std::map<std::string, double>& indexQuantities,
    std::map<std::string, boost::shared_ptr<QuantExt::FxIndex>>& fxIndices, Real& initialPrice,
    std::string& assetCurrency, std::string& creditRiskCurrency,
    std::map<std::string, SimmCreditQualifierMapping>& creditQualifierMapping, Date& maturity,
    const std::function<boost::shared_ptr<QuantExt::FxIndex>(
        const boost::shared_ptr<Market> market, const std::string& configuration, const std::string& domestic,
        const std::string& foreign, std::map<std::string, boost::shared_ptr<QuantExt::FxIndex>>& fxIndices)>&
        getFxIndex,
    const std::string& underlyingDerivativeId) const {
    auto t = boost::dynamic_pointer_cast<ore::data::Bond>(underlying);
    QL_REQUIRE(t, "could not cast to ore::data::Bond, this is unexpected");
    auto qlBond = boost::dynamic_pointer_cast<QuantLib::Bond>(underlying->instrument()->qlInstrument());
    QL_REQUIRE(qlBond, "expected QuantLib::Bond, could not cast");
    underlyingIndex = boost::make_shared<QuantExt::BondIndex>(
        t->bondData().securityId(), true, false, NullCalendar(), qlBond, Handle<YieldTermStructure>(),
        Handle<DefaultProbabilityTermStructure>(), Handle<Quote>(), Handle<Quote>(), Handle<YieldTermStructure>(), true,
        t->bondData().priceQuoteMethod(), t->bondData().priceQuoteBaseValue(), t->bondData().isInflationLinked());
    underlyingMultiplier = t->bondData().bondNotional();
    indexQuantities["BOND-" + t->bondData().securityId()] = underlyingMultiplier;
    Real adj = t->bondData().priceQuoteMethod() == QuantExt::BondIndex::PriceQuoteMethod::CurrencyPerUnit
                   ? 1.0 / t->bondData().priceQuoteBaseValue()
                   : 1.0;
    DLOG("BondTrsUnderlyingBuilder: price quote method adjustment for " << t->bondData().securityId() << " is " << adj);
    if (initialPrice != Null<Real>())
        initialPrice = initialPrice * qlBond->notional(valuationDates.front()) * adj;
    assetCurrency = t->bondData().currency();
    if (!t->bondData().creditCurveId().empty())
        creditRiskCurrency = t->bondData().currency();
    creditQualifierMapping[securitySpecificCreditCurveName(t->bondData().securityId(), t->bondData().creditCurveId())] =
        SimmCreditQualifierMapping(t->bondData().securityId(), t->bondData().creditGroup());
    creditQualifierMapping[t->bondData().creditCurveId()] =
        SimmCreditQualifierMapping(t->bondData().securityId(), t->bondData().creditGroup());
    // FIXME shouldn't we leave that empty and let TRS determine the maturity date based on valuation / funding dates?
    maturity = qlBond->maturityDate();
}

void ForwardBondTrsUnderlyingBuilder::build(
    const std::string& parentId, const boost::shared_ptr<Trade>& underlying, const std::vector<Date>& valuationDates,
    const boost::shared_ptr<EngineFactory>& engineFactory, boost::shared_ptr<QuantLib::Index>& underlyingIndex,
    Real& underlyingMultiplier, std::map<std::string, double>& indexQuantities,
    std::map<std::string, boost::shared_ptr<QuantExt::FxIndex>>& fxIndices, Real& initialPrice,
    std::string& assetCurrency, std::string& creditRiskCurrency,
    std::map<std::string, SimmCreditQualifierMapping>& creditQualifierMapping, Date& maturity,
    const std::function<boost::shared_ptr<QuantExt::FxIndex>(
        const boost::shared_ptr<Market> market, const std::string& configuration, const std::string& domestic,
        const std::string& foreign, std::map<std::string, boost::shared_ptr<QuantExt::FxIndex>>& fxIndices)>&
        getFxIndex,
    const std::string& underlyingDerivativeId) const {
    auto t = boost::dynamic_pointer_cast<ore::data::ForwardBond>(underlying);
    QL_REQUIRE(t, "could not cast to ore::data::ForwardBond, this is unexpected");
    auto qlBond = boost::dynamic_pointer_cast<QuantExt::ForwardBond>(underlying->instrument()->qlInstrument());
    QL_REQUIRE(qlBond, "expected QuantExt::ForwardBond, could not cast");
    underlyingIndex = boost::make_shared<QuantExt::BondFuturesIndex>(
        parseDate(t->fwdMaturityDate()), t->bondData().securityId(), true, false, NullCalendar(), qlBond->underlying());
    underlyingMultiplier = t->bondData().bondNotional();

    std::ostringstream o;
    o << "BOND-" + t->bondData().securityId() << "-" << QuantLib::io::iso_date(parseDate(t->fwdMaturityDate()));
    std::string name = o.str();
    name.erase(name.length() - 3);
    indexQuantities[name] = underlyingMultiplier;

    Real adj = t->bondData().priceQuoteMethod() == QuantExt::BondIndex::PriceQuoteMethod::CurrencyPerUnit
                   ? 1.0 / t->bondData().priceQuoteBaseValue()
                   : 1.0;
    if (initialPrice != Null<Real>())
        initialPrice = initialPrice * qlBond->underlying()->notional(valuationDates.front()) * adj;
    assetCurrency = t->bondData().currency();
    if (!t->bondData().creditCurveId().empty())
        creditRiskCurrency = t->bondData().currency();
    creditQualifierMapping[securitySpecificCreditCurveName(t->bondData().securityId(), t->bondData().creditCurveId())] =
        SimmCreditQualifierMapping(t->bondData().securityId(), t->bondData().creditGroup());
    creditQualifierMapping[t->bondData().creditCurveId()] =
        SimmCreditQualifierMapping(t->bondData().securityId(), t->bondData().creditGroup());
}

template <class T>
void AssetPositionTrsUnderlyingBuilder<T>::build(
    const std::string& parentId, const boost::shared_ptr<Trade>& underlying, const std::vector<Date>& valuationDates,
    const boost::shared_ptr<EngineFactory>& engineFactory, boost::shared_ptr<QuantLib::Index>& underlyingIndex,
    Real& underlyingMultiplier, std::map<std::string, double>& indexQuantities,
    std::map<std::string, boost::shared_ptr<QuantExt::FxIndex>>& fxIndices, Real& initialPrice,
    std::string& assetCurrency, std::string& creditRiskCurrency,
    std::map<std::string, SimmCreditQualifierMapping>& creditQualifierMapping, Date& maturity,
    const std::function<boost::shared_ptr<QuantExt::FxIndex>(
        const boost::shared_ptr<Market> market, const std::string& configuration, const std::string& domestic,
        const std::string& foreign, std::map<std::string, boost::shared_ptr<QuantExt::FxIndex>>& fxIndices)>&
        getFxIndex,
    const std::string& underlyingDerivativeId) const {
    auto t = boost::dynamic_pointer_cast<T>(underlying);
    QL_REQUIRE(t, "could not cast to ore::data::EquityPosition, this is unexpected");
    if (t->isSingleCurrency()) {
        assetCurrency = t->npvCurrency();
        DLOG("underlying equity position is single-currency, assetCurrency is " << assetCurrency);
    } else {
        // asset currency is set to funding currency data currency in trs as a default
        // we use fxSpot() as opposed to fxRate() here to ensure consistency between NPV() and the fixing of an
        // equivalent index representing the same basket
        t->setNpvCurrencyConversion(
            assetCurrency, engineFactory->market()->fxSpot(t->npvCurrency() + assetCurrency,
                                                           engineFactory->configuration(MarketContext::pricing)));
        DLOG("underlying equity position is multi-currency, set assetCurrency to fundingCurrency = " << assetCurrency);
    }
    std::vector<boost::shared_ptr<QuantExt::FxIndex>> fxConversion(t->data().underlyings().size());
    std::vector<boost::shared_ptr<QuantLib::Index>> indices;
    for (auto const& i : t->indices()) {
        indices.push_back(i);
        DLOG("underlying equity index " << i->name() << " added.");
    }
    for (Size i = 0; i < t->data().underlyings().size(); ++i) {
        fxConversion[i] = getFxIndex(engineFactory->market(), engineFactory->configuration(MarketContext::pricing),
                                     assetCurrency, getIndexCurrencyFromPosition(t, i), fxIndices);
        updateQuantities(indexQuantities, t->data().underlyings()[i].name(),
                         t->weights()[i] * t->data().quantity());
    }
    underlyingIndex = boost::make_shared<QuantExt::CompositeIndex>("Composite Index trade id " + parentId, indices,
                                                                   t->weights(), fxConversion);
    DLOG("underlying equity index built with " << indices.size() << " constituents.");
    underlyingMultiplier = t->data().quantity();
}

template <>
std::string AssetPositionTrsUnderlyingBuilder<ore::data::EquityPosition>::getIndexCurrencyFromPosition(
    boost::shared_ptr<EquityPosition> position, size_t i) const {
    return position->indices()[i]->currency().code();
}

template <>
std::string AssetPositionTrsUnderlyingBuilder<ore::data::CommodityPosition>::getIndexCurrencyFromPosition(
    boost::shared_ptr<CommodityPosition> position, size_t i) const {
    return position->indices()[i]->priceCurve()->currency().code();
}

template <class T>
std::string AssetPositionTrsUnderlyingBuilder<T>::getIndexCurrencyFromPosition(
    boost::shared_ptr<T> position, size_t i) const {
    QL_FAIL("internal error AssetPositionTrsUnderlyingBuilder only support Equity and Commodity positions");
}

template<>
void AssetPositionTrsUnderlyingBuilder<ore::data::EquityPosition>::updateQuantities(std::map<std::string, double>& indexQuantities, const std::string& indexName, const double qty) const {
    indexQuantities["EQ-" + indexName] = qty;
}

template <>
void AssetPositionTrsUnderlyingBuilder<ore::data::CommodityPosition>::updateQuantities(
    std::map<std::string, double>& indexQuantities, const std::string& indexName, const double qty) const {
    indexQuantities["COMM-" + indexName] = qty;
}

template <class T>
void AssetPositionTrsUnderlyingBuilder<T>::updateQuantities(std::map<std::string, double>& indexQuantities,
                                                            const std::string& indexName, const double qty) const {
    QL_FAIL("internal error AssetPositionTrsUnderlyingBuilder only support Equity and Commodity positions");
}

void EquityOptionPositionTrsUnderlyingBuilder::build(
    const std::string& parentId, const boost::shared_ptr<Trade>& underlying, const std::vector<Date>& valuationDates,
    const boost::shared_ptr<EngineFactory>& engineFactory, boost::shared_ptr<QuantLib::Index>& underlyingIndex,
    Real& underlyingMultiplier, std::map<std::string, double>& indexQuantities,
    std::map<std::string, boost::shared_ptr<QuantExt::FxIndex>>& fxIndices, Real& initialPrice,
    std::string& assetCurrency, std::string& creditRiskCurrency,
    std::map<std::string, SimmCreditQualifierMapping>& creditQualifierMapping, Date& maturity,
    const std::function<boost::shared_ptr<QuantExt::FxIndex>(
        const boost::shared_ptr<Market> market, const std::string& configuration, const std::string& domestic,
        const std::string& foreign, std::map<std::string, boost::shared_ptr<QuantExt::FxIndex>>& fxIndices)>&
        getFxIndex,
    const std::string& underlyingDerivativeId) const {
    auto t = boost::dynamic_pointer_cast<ore::data::EquityOptionPosition>(underlying);
    QL_REQUIRE(t, "could not cast to ore::data::EquityOptionPosition, this is unexpected");
    if (t->isSingleCurrency()) {
        assetCurrency = t->npvCurrency();
        DLOG("underlying equity option position is single-currency, assetCurrency is " << assetCurrency);
    } else {
        // asset currency is set to funding currency data currency in trs as a default
        // we use fxSpot() as opposed to fxRate() here to ensure consistency between NPV() and the fixing of an
        // equivalent index representing the same basket
        t->setNpvCurrencyConversion(
            assetCurrency, engineFactory->market()->fxSpot(t->npvCurrency() + assetCurrency,
                                                           engineFactory->configuration(MarketContext::pricing)));
        DLOG("underlying equity option position is multi-currency, set assetCurrency to fundingCurrency = "
             << assetCurrency);
    }
    std::vector<boost::shared_ptr<QuantExt::FxIndex>> fxConversion(t->data().underlyings().size());
    std::vector<boost::shared_ptr<QuantLib::Index>> indices;
    for (auto const& i : t->historicalPriceIndices()) {
        indices.push_back(i);
        DLOG("underlying historical equity option price index " << i->name() << " added.");
    }
    QL_REQUIRE(indices.size() == t->data().underlyings().size(),
               "underlying historical price indices size (" << indices.size() << ") must match underlyings size ("
                                                            << t->data().underlyings().size());
    for (Size i = 0; i < t->data().underlyings().size(); ++i) {
        fxConversion[i] = getFxIndex(engineFactory->market(), engineFactory->configuration(MarketContext::pricing),
                                     assetCurrency, t->currencies()[i], fxIndices);
        indexQuantities[indices[i]->name()] = t->weights()[i] * t->positions()[i];
    }
    std::vector<Real> w;
    for (Size i = 0; i < t->weights().size(); ++i) {
        w.push_back(t->weights()[i] * t->positions()[i]);
    }
    underlyingIndex =
        boost::make_shared<QuantExt::CompositeIndex>("Composite Index trade id " + parentId, indices, w, fxConversion);
    DLOG("underlying equity option historical price index built with " << indices.size() << " constituents.");
    underlyingMultiplier = t->data().quantity();
}

void BondPositionTrsUnderlyingBuilder::build(
    const std::string& parentId, const boost::shared_ptr<Trade>& underlying, const std::vector<Date>& valuationDates,
    const boost::shared_ptr<EngineFactory>& engineFactory, boost::shared_ptr<QuantLib::Index>& underlyingIndex,
    Real& underlyingMultiplier, std::map<std::string, double>& indexQuantities,
    std::map<std::string, boost::shared_ptr<QuantExt::FxIndex>>& fxIndices, Real& initialPrice,
    std::string& assetCurrency, std::string& creditRiskCurrency,
    std::map<std::string, SimmCreditQualifierMapping>& creditQualifierMapping, Date& maturity,
    const std::function<boost::shared_ptr<QuantExt::FxIndex>(
        const boost::shared_ptr<Market> market, const std::string& configuration, const std::string& domestic,
        const std::string& foreign, std::map<std::string, boost::shared_ptr<QuantExt::FxIndex>>& fxIndices)>&
        getFxIndex,
    const std::string& underlyingDerivativeId) const {
    auto t = boost::dynamic_pointer_cast<ore::data::BondPosition>(underlying);
    QL_REQUIRE(t, "could not cast to ore::data::BondPosition, this is unexpected");
    if (t->isSingleCurrency()) {
        assetCurrency = t->npvCurrency();
        DLOG("underlying bond position is single-currency, assetCurrency is " << assetCurrency);
    } else {
        // asset currency is set to funding currency data currency in trs as a default
        t->setNpvCurrencyConversion(
            assetCurrency, engineFactory->market()->fxSpot(t->npvCurrency() + assetCurrency,
                                                           engineFactory->configuration(MarketContext::pricing)));
        DLOG("underlying bond position is multi-currency, set assetCurrency to fundingCurrency = " << assetCurrency);
    }

    std::vector<boost::shared_ptr<QuantExt::FxIndex>> fxConversion(t->data().underlyings().size());
    std::vector<boost::shared_ptr<QuantLib::Index>> indices;
    bool hasCreditRisk = false;
    for (Size i = 0; i < t->bonds().size(); ++i) {
        // relative index, because weights are supposed to include any amortization factors
        indices.push_back(boost::make_shared<QuantExt::BondIndex>(
            t->data().underlyings()[i].name(), true, true, NullCalendar(), t->bonds()[i].bond,
            Handle<YieldTermStructure>(), Handle<DefaultProbabilityTermStructure>(), Handle<Quote>(), Handle<Quote>(),
            Handle<YieldTermStructure>(), true, t->bonds()[i].priceQuoteMethod, t->bonds()[i].priceQuoteBaseValue,
            t->bonds()[i].isInflationLinked, t->data().underlyings()[i].bidAskAdjustment()));
        DLOG("underlying bond index " << indices.back()->name() << " added.");
        indexQuantities["BOND-" + t->data().underlyings()[i].name()] = t->weights()[i] * t->data().quantity();
        creditQualifierMapping[ore::data::securitySpecificCreditCurveName(t->bonds()[i].securityId,
                                                                          t->bonds()[i].creditCurveId)] =
            SimmCreditQualifierMapping(t->bonds()[i].securityId, t->bonds()[i].creditGroup);
        creditQualifierMapping[t->bonds()[i].creditCurveId] =
            SimmCreditQualifierMapping(t->bonds()[i].securityId, t->bonds()[i].creditGroup);
        hasCreditRisk = hasCreditRisk || t->bonds()[i].hasCreditRisk;
    }
    for (Size i = 0; i < t->data().underlyings().size(); ++i) {
        fxConversion[i] = getFxIndex(engineFactory->market(), engineFactory->configuration(MarketContext::pricing),
                                     assetCurrency, t->bonds()[i].currency, fxIndices);
    }
    std::vector<Real> w;
    for (Size i = 0; i < t->weights().size(); ++i) {
        w.push_back(t->weights()[i]);
    }
    underlyingIndex =
        boost::make_shared<QuantExt::CompositeIndex>("Composite Index trade id " + parentId, indices, w, fxConversion);
    DLOG("underlying bond position index built with " << indices.size() << " constituents.");
    underlyingMultiplier = t->data().quantity();
    if (hasCreditRisk)
        creditRiskCurrency = assetCurrency;
    // FIXME same question as for single bond underlying: shouldn't we leave that empty and let TRS determine the
    // maturity date based on valuation / funding dates?
    maturity = t->maturity();
}

void DerivativeTrsUnderlyingBuilder::build(
    const std::string& parentId, const boost::shared_ptr<Trade>& underlying, const std::vector<Date>& valuationDates,
    const boost::shared_ptr<EngineFactory>& engineFactory, boost::shared_ptr<QuantLib::Index>& underlyingIndex,
    Real& underlyingMultiplier, std::map<std::string, double>& indexQuantities,
    std::map<std::string, boost::shared_ptr<QuantExt::FxIndex>>& fxIndices, Real& initialPrice,
    std::string& assetCurrency, std::string& creditRiskCurrency,
    std::map<std::string, SimmCreditQualifierMapping>& creditQualifierMapping, Date& maturity,
    const std::function<boost::shared_ptr<QuantExt::FxIndex>(
        const boost::shared_ptr<Market> market, const std::string& configuration, const std::string& domestic,
        const std::string& foreign, std::map<std::string, boost::shared_ptr<QuantExt::FxIndex>>& fxIndices)>&
        getFxIndex,
    const std::string& underlyingDerivativeId) const {
    assetCurrency = underlying->npvCurrency();
    underlyingIndex = boost::make_shared<QuantExt::GenericIndex>("GENERIC-" + underlyingDerivativeId);
    indexQuantities["GENERIC-" + underlyingDerivativeId] = 1.0;
    underlyingMultiplier = 1.0;
    // FIXME same question as for single bond underlying: shouldn't we leave that empty and let TRS determine the
    // maturity date based on valuation / funding dates?
    maturity = underlying->maturity();
}

template struct AssetPositionTrsUnderlyingBuilder<ore::data::EquityPosition>;
template struct AssetPositionTrsUnderlyingBuilder<ore::data::CommodityPosition>;

} // namespace data
} // namespace ore
