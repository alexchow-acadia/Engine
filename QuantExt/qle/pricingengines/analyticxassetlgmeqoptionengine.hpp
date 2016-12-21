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

/*! \file analyticcclgmfxoptionengine.hpp
    \brief analytic cc lgm fx option engine

        \ingroup engines
*/

#ifndef quantext_xassetlgm_eqoptionengine_hpp
#define quantext_xassetlgm_eqoptionengine_hpp

#include <qle/models/crossassetmodel.hpp>
#include <ql/instruments/vanillaoption.hpp>

namespace QuantExt {

//! Analytic cross-asset lgm equity option engine
/*! \ingroup engines
*/
class AnalyticXAssetLgmEquityOptionEngine : public VanillaOption::engine {
public:
    AnalyticXAssetLgmEquityOptionEngine(
        const boost::shared_ptr<CrossAssetModel>& model,
        const Size equityIdx,
        const Size ccyIdx);
    void calculate() const;

    /*! if cache is enabled, the integrals independent of fx
      volatility are cached, which can speed up calibtration;
      remember to flush the cache when the ir parameters
      change, this can be done by another call to cache */
    void cache(bool enable = true);

    /*! the actual option price calculation, exposed to public,
      since it is useful to directly use the core computation
      sometimes */
    Real value(const Time t0, const Time t, const boost::shared_ptr<StrikedTypePayoff> payoff,
               const Real domesticDiscount, const Real eqForward) const;

private:
    const boost::shared_ptr<CrossAssetModel> model_;
    const Size eqIdx_, ccyIdx_;
    bool cacheEnabled_;
    mutable bool cacheDirty_;
    mutable Real cachedIntegrals_, cachedT0_, cachedT_;
};

// inline

inline void AnalyticXAssetLgmEquityOptionEngine::cache(bool enable) {
    cacheEnabled_ = enable;
    cacheDirty_ = true;
}

} // namespace QuantExt

#endif
