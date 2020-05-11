/*
 Copyright (C) 2016 Quaternion Risk Management Ltd
 Copyright (C) 2017 Aareal Bank AG

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

#include <boost/lexical_cast.hpp>
#include <ored/portfolio/bond.hpp>
#include <ored/portfolio/bondutils.hpp>
#include <ored/portfolio/builders/bond.hpp>
#include <ored/portfolio/fixingdates.hpp>
#include <ored/portfolio/legdata.hpp>
#include <ored/portfolio/swap.hpp>
#include <ored/utilities/indexparser.hpp>
#include <ored/utilities/log.hpp>
#include <ored/utilities/parsers.hpp>
#include <ql/cashflows/simplecashflow.hpp>
#include <ql/instruments/bond.hpp>
#include <ql/instruments/bonds/zerocouponbond.hpp>

using namespace QuantLib;
using namespace QuantExt;

namespace ore {
namespace data {

void BondData::fromXML(XMLNode* node) {
    XMLUtils::checkNode(node, "BondData");
    QL_REQUIRE(node, "No BondData Node");
    issuerId_ = XMLUtils::getChildValue(node, "IssuerId", false);
    creditCurveId_ = XMLUtils::getChildValue(node, "CreditCurveId", false);
    securityId_ = XMLUtils::getChildValue(node, "SecurityId", true);
    referenceCurveId_ = XMLUtils::getChildValue(node, "ReferenceCurveId", false);
    incomeCurveId_ = XMLUtils::getChildValue(node, "IncomeCurveId", false);
    volatilityCurveId_ = XMLUtils::getChildValue(node, "VolatilityCurveId", false);
    settlementDays_ = XMLUtils::getChildValue(node, "SettlementDays", false);
    calendar_ = XMLUtils::getChildValue(node, "Calendar", false);
    issueDate_ = XMLUtils::getChildValue(node, "IssueDate", false);
    if (auto n = XMLUtils::getChildNode(node, "BondNotional")) {
        bondNotional_ = parseReal(XMLUtils::getNodeValue(n));
    } else {
        bondNotional_ = 1.0;
    }
    XMLNode* legNode = XMLUtils::getChildNode(node, "LegData");
    while (legNode != nullptr) {
        LegData ld;
        ld.fromXML(legNode);
        coupons_.push_back(ld);
        legNode = XMLUtils::getNextSibling(legNode, "LegData");
    }
}

XMLNode* BondData::toXML(XMLDocument& doc) {
    XMLNode* bondNode = doc.allocNode("BondData");
    XMLUtils::addChild(doc, bondNode, "IssuerId", issuerId_);
    XMLUtils::addChild(doc, bondNode, "CreditCurveId", creditCurveId_);
    XMLUtils::addChild(doc, bondNode, "SecurityId", securityId_);
    XMLUtils::addChild(doc, bondNode, "ReferenceCurveId", referenceCurveId_);
    if (!incomeCurveId_.empty())
        XMLUtils::addChild(doc, bondNode, "IncomeCurveId", incomeCurveId_);
    if (!volatilityCurveId_.empty())
        XMLUtils::addChild(doc, bondNode, "VolatilityCurveId", volatilityCurveId_);
    XMLUtils::addChild(doc, bondNode, "SettlementDays", settlementDays_);
    XMLUtils::addChild(doc, bondNode, "Calendar", calendar_);
    XMLUtils::addChild(doc, bondNode, "IssueDate", issueDate_);
    XMLUtils::addChild(doc, bondNode, "BondNotional", bondNotional_);
    for (auto& c : coupons_)
        XMLUtils::appendNode(bondNode, c.toXML(doc));
    return bondNode;
}

void BondData::populateFromBondReferenceData(const boost::shared_ptr<ReferenceDataManager>& referenceData) {
    ore::data::populateFromBondReferenceData(issuerId_, settlementDays_, calendar_, issueDate_, creditCurveId_,
                                             referenceCurveId_, incomeCurveId_, volatilityCurveId_, coupons_,
                                             securityId_, referenceData);
    // plausibility check
    QL_REQUIRE(!settlementDays_.empty(),
               "settlement days not given, check bond trade xml and reference data for '" << securityId_ << "'");
}

void Bond::build(const boost::shared_ptr<EngineFactory>& engineFactory) {
    DLOG("Bond::build() called for trade " << id());

    const boost::shared_ptr<Market> market = engineFactory->market();
    boost::shared_ptr<EngineBuilder> builder = engineFactory->builder("Bond");
    bondData_.populateFromBondReferenceData(engineFactory->referenceData());

    Date issueDate = parseDate(bondData_.issueDate());
    Calendar calendar = parseCalendar(bondData_.calendar());
    Natural settlementDays = boost::lexical_cast<Natural>(bondData_.settlementDays());
    boost::shared_ptr<QuantLib::Bond> bond;

    // FIXME: zero bonds are always long (firstLegIsPayer = false, mult = 1.0)
    bool firstLegIsPayer = (bondData_.coupons().size() == 0) ? false : bondData_.coupons()[0].isPayer();
    Real mult = bondData_.bondNotional() * (firstLegIsPayer ? -1.0 : 1.0);
    std::vector<Leg> separateLegs;
    if (bondData_.zeroBond()) { // Zero coupon bond
        bond.reset(new QuantLib::ZeroCouponBond(settlementDays, calendar, bondData_.faceAmount(),
                                                parseDate(bondData_.maturityDate())));
        currency_ = bondData_.currency();
    } else { // Coupon bond
        for (Size i = 0; i < bondData_.coupons().size(); ++i) {
            bool legIsPayer = bondData_.coupons()[i].isPayer();
            QL_REQUIRE(legIsPayer == firstLegIsPayer, "Bond legs must all have same pay/receive flag");
            if (i == 0)
                currency_ = bondData_.coupons()[i].currency();
            else {
                QL_REQUIRE(currency_ == bondData_.coupons()[i].currency(),
                           "leg #" << i << " currency (" << bondData_.coupons()[i].currency()
                                   << ") not equal to leg #0 currency (" << bondData_.coupons()[0].currency());
            }
            Leg leg;
            auto configuration = builder->configuration(MarketContext::pricing);
            auto legBuilder = engineFactory->legBuilder(bondData_.coupons()[i].legType());
            leg = legBuilder->buildLeg(bondData_.coupons()[i], engineFactory, requiredFixings_, configuration);
            separateLegs.push_back(leg);
        } // for coupons_
        Leg leg = joinLegs(separateLegs);
        bond.reset(new QuantLib::Bond(settlementDays, calendar, issueDate, leg));
        // workaround, QL doesn't register a bond with its leg's cashflows
        for (auto const& c : leg)
            bond->registerWith(c);
    }

    Currency currency = parseCurrency(currency_);
    boost::shared_ptr<BondEngineBuilder> bondBuilder = boost::dynamic_pointer_cast<BondEngineBuilder>(builder);
    QL_REQUIRE(bondBuilder, "No Builder found for Bond: " << id());
    bond->setPricingEngine(
        bondBuilder->engine(currency, bondData_.creditCurveId(), bondData_.securityId(), bondData_.referenceCurveId()));
    instrument_.reset(new VanillaInstrument(bond, mult));

    npvCurrency_ = currency_;
    maturity_ = bond->cashflows().back()->date();
    notional_ = currentNotional(bond->cashflows());
    notionalCurrency_ = currency_;

    // Add legs (only 1)
    legs_ = {bond->cashflows()};
    legCurrencies_ = {npvCurrency_};
    legPayers_ = {firstLegIsPayer};
}

void Bond::fromXML(XMLNode* node) {
    Trade::fromXML(node);
    bondData_.fromXML(XMLUtils::getChildNode(node, "BondData"));
}

XMLNode* Bond::toXML(XMLDocument& doc) {
    XMLNode* node = Trade::toXML(doc);
    XMLUtils::appendNode(node, bondData_.toXML(doc));
    return node;
}
} // namespace data
} // namespace ore
