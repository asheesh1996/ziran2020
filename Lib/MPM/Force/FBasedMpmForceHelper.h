#ifndef F_BASED_MPM_FORCE_HELPER_H
#define F_BASED_MPM_FORCE_HELPER_H

#include "MpmForceHelperBase.h"

#include <Ziran/CS/Util/Forward.h>
#include <Ziran/CS/DataStructure/DataArray.h>
#include <Ziran/CS/DataStructure/DataManager.h>
#include <Ziran/Math/Geometry/Particles.h>

namespace ZIRAN {

template <class T, int dim>
class MpmForceHelperBase;

template <class TCONST>
class FBasedMpmForceHelper : public MpmForceHelperBase<typename TCONST::Scalar, TCONST::TM::RowsAtCompileTime> {
public:
    static const int dim = TCONST::TM::RowsAtCompileTime;
    using T = typename TCONST::Scalar;
    using Scratch = typename TCONST::Scratch;
    typedef Matrix<T, dim, dim> TM;
    typedef Vector<T, dim> TV;
    using TVStack = Matrix<T, dim, Eigen::Dynamic>;

    Particles<T, dim>& particles;
    DataArray<T>& element_measure;
    DataArray<TCONST>& constitutive_model;
    DataArray<Scratch>& scratch;
    DataArray<TM>& F;

    StdVector<TM> Fn;

    explicit FBasedMpmForceHelper(Particles<T, dim>& particles)
        : particles(particles)
        , element_measure(particles.add(element_measure_name()))
        , constitutive_model(particles.add(constitutive_model_name()))
        , scratch(particles.add(constitutive_model_scratch_name()))
        , F(particles.add(F_name()))
    {
    }
    virtual ~FBasedMpmForceHelper() {}

    void reinitialize() override;

    void backupStrain() override;

    void restoreStrain() override;

    // add stress to vtau (called only by symplectic)
    void updateState(const DisjointRanges& subrange, StdVector<TM>& vtau, TVStack& fp) override;

    // add stress to vPFnT (called only by implicit)
    void updateImplicitState(const DisjointRanges& subrange, StdVector<TM>& vPFnT, TVStack& fp) override;

    void evolveStrain(const DisjointRanges& subrange, T dt, const StdVector<TM>& gradV) override;

    double totalEnergy(const DisjointRanges& subrange) override;

    void computeStressDifferential(const DisjointRanges& subrange, const StdVector<TM>& gradDv, StdVector<TM>& dstress, const TVStack& dvp, TVStack& dfp) override;

    template <class TCCONST, class TPCONST>
    void computeStressDifferentialWithPlasticity(const DisjointRanges& subrange, const StdVector<TM>& gradDv, StdVector<TM>& dstress)
    {
        auto c_name = AttributeName<TCCONST>(TCCONST::name());
        auto p_name = AttributeName<TPCONST>(TPCONST::name());
        auto ranges = particles.X.ranges;
        if (!particles.exist(c_name) || !particles.exist(p_name))
            return;
        DisjointRanges subset(subrange, particles.commonRanges(c_name, element_measure_name(), F_name(), p_name));
        if (subset.size() == 0)
            return;
        for (auto iter = particles.subsetIter(subset, c_name, F_name(), element_measure_name(), valueIdOnly(F_name()), p_name); iter; ++iter) {
            auto& constitutive_model = iter.template get<0>();
            auto& F = iter.template get<1>();
            auto& element_measure = iter.template get<2>();
            int F_index = iter.template get<3>();
            auto& plasticity_model = iter.template get<4>();
            int p = iter.entryId();
            auto& Fn_local = Fn[F_index];
            TM dP;
            TM dF = gradDv[p] * Fn_local;

            TM U, V;
            TV sigma;
            singularValueDecomposition(F, U, sigma, V);

            TM test_matrix = plasticity_model.projectSigmaDerivative(constitutive_model, sigma);
            if (test_matrix == TM::Zero()) {
                continue;
            }
            else if (test_matrix == TM::Identity()) {
                TM A = constitutive_model.firstPiolaDerivativeDiagonal(sigma);
                if constexpr (dim == 2) {
                    T clamp_value = 1e-8;
                    TM B01 = constitutive_model.Bij(sigma, 0, 1, clamp_value);
                    Matrix<T, 4, 4> M_hat = Matrix<T, 4, 4>::Zero();
                    M_hat(0, 0) = A(0, 0);
                    M_hat(0, 3) = A(0, 1);
                    M_hat.block(1, 1, 2, 2) = B01;
                    M_hat(3, 0) = A(1, 0);
                    M_hat(3, 3) = A(1, 1);

                    TM T2 = U.transpose() * dF * V;
                    Vector<T, 4> T3_flatten = (M_hat * Vector<T, 4>(T2.data())).transpose();
                    TM T3 = TM(T3_flatten.data());
                    dP = U * T3 * V.transpose();
                }
                else {
                    ZIRAN_ASSERT(false, "not implemented");
                }
            }
            else {
                TV zi = plasticity_model.projectSigma(constitutive_model, sigma);
                TM A = constitutive_model.firstPiolaDerivativeDiagonal(zi) * plasticity_model.projectSigmaDerivative(constitutive_model, sigma);
                if constexpr (dim == 2) {
                    T clamp_value = 1e-8;
                    T st = plasticity_model.secondTerm(constitutive_model, sigma, 0, 1);
                    TM B01 = constitutive_model.BijFull(zi, sigma, st, 0, 1, clamp_value);
                    Matrix<T, 4, 4> M_hat = Matrix<T, 4, 4>::Zero();
                    M_hat(0, 0) = A(0, 0);
                    M_hat(0, 3) = A(0, 1);
                    M_hat.block(1, 1, 2, 2) = B01;
                    M_hat(3, 0) = A(1, 0);
                    M_hat(3, 3) = A(1, 1);

                    TM T2 = U.transpose() * dF * V;
                    Vector<T, 4> T3_flatten = (M_hat * Vector<T, 4>(T2.data())).transpose();
                    TM T3 = TM(T3_flatten.data());
                    dP = U * T3 * V.transpose();
                }
                else {
                    ZIRAN_ASSERT(false, "not implemented");
                }
            }
            assert(dP == dP);
            dstress[p] += dP * element_measure * Fn_local.transpose();
        }
    }

    using Hessian = Eigen::Matrix<T, dim * dim, dim * dim>;
    void runLambdaWithDifferential(
        const StdVector<int>& particle_order,
        const std::vector<std::pair<int, int>>& particle_group,
        const std::vector<uint64_t>& block_offset,
        std::function<void(int, const Hessian&, const TM&, const T&, const T&, bool)> func, int opt = 0) override
    {
        std::vector<int> model_local_idx(particles.count, -1);
        std::vector<int> F_local_idx(particles.count, -1);
        static std::vector<Hessian> dPdF(particles.count);
        if (opt == 1) {
            if ((int)dPdF.size() != particles.count)
                dPdF.resize(particles.count);
        }
        tbb::parallel_for(particles.X.ranges, [&](DisjointRanges& subrange) {
            DisjointRanges subset(subrange, particles.commonRanges(constitutive_model_name(), F_name()));
            for (auto iter = particles.subsetIter(subset, constitutive_model_name(), valueIdOnly(constitutive_model_name()), F_name(), valueIdOnly(F_name())); iter; ++iter) {
                model_local_idx[iter.entryId()] = iter.template get<1>();
                F_local_idx[iter.entryId()] = iter.template get<3>();
            }
        });

        auto* F_pointer = &particles.DataManager::get(F_name());
        auto* model_pointer = &particles.DataManager::get(constitutive_model_name());
        auto* scratch_pointer = &particles.DataManager::get(constitutive_model_scratch_name());
        for (uint64_t color = 0; color < (1 << dim); ++color)
            tbb::parallel_for(0, (int)particle_group.size(), [&](int group_idx) {
                if ((block_offset[group_idx] & ((1 << dim) - 1)) != color)
                    return;
                for (int idx = particle_group[group_idx].first; idx <= particle_group[group_idx].second; ++idx) {
                    int i = particle_order[idx];
                    // not the same model
                    int model_idx = model_local_idx[i];
                    int F_idx = F_local_idx[i];
                    if (model_idx < 0)
                        continue;
                    auto& F = (*F_pointer)[F_idx];
                    auto& model = (*model_pointer)[model_idx];
                    auto& Fn_local = Fn[F_idx];
                    auto& s = (*scratch_pointer)[model_idx];
                    Hessian firstPiolaDerivative;
                    if (opt < 2) {
                        model.updateScratch(F, s);
                        model.firstPiolaDerivative(s, firstPiolaDerivative);
                        if (opt == 1)
                            dPdF[i] = firstPiolaDerivative;
                    }
                    else if (opt == 2) {
#if 0
                        model.updateScratch(F, s);
                        model.firstPiolaDerivative(s, firstPiolaDerivative);
                        if (i < 20)
                            ZIRAN_INFO(i, "-th particle dPdF diff norm ", (dPdF[i] - firstPiolaDerivative).norm());
#endif
                        firstPiolaDerivative = dPdF[i];
                    }
                    func(i, firstPiolaDerivative, Fn_local, (T)-1, (T)-1, false);
                }
            });
    }

    void computeStressDifferentialWithPlasticity(const DisjointRanges& subrange, const StdVector<TM>& gradDv, StdVector<TM>& dstress) override
    {
        computeStressDifferentialWithPlasticity<StvkWithHencky<T, dim>, DruckerPragerStvkHencky<T>>(subrange, gradDv, dstress);
    }

    inline virtual AttributeName<TCONST> constitutive_model_name()
    {
        return AttributeName<TCONST>(TCONST::name());
    }
    inline virtual AttributeName<Scratch> constitutive_model_scratch_name()
    {
        return AttributeName<Scratch>(TCONST::scratch_name());
    }
    inline static AttributeName<T> element_measure_name()
    {
        return AttributeName<T>("element measure");
    }
    inline virtual AttributeName<TM> F_name()
    {
        return AttributeName<TM>("F");
    }
};
} // namespace ZIRAN
#endif /* ifndef F_BASED_MPM_FORCE_HELPER */
