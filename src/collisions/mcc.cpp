#include "kn/collisions/mcc.h"
#include "kn/constants/constants.h"
#include "kn/particle/species.h"
#include "kn/random/random.h"
#include "kn/core/vec.h"

#include <unordered_map>
#include <utility>
#include <cmath>
#include <algorithm>

using namespace kn::collisions;

namespace {
    double interpolate_cross_section(const MonteCarloCollisions::CollisionReaction& cs, double energy) {
        if(energy <= cs.energy.front())
            return cs.cross_section.front();
        else if(energy >= cs.energy.back())
            return cs.cross_section.back();
        else {
            auto it = std::lower_bound(cs.energy.begin(), cs.energy.end(), energy);
            
            size_t rhs = it - cs.energy.begin();

            double x0 = cs.energy[rhs - 1];
            double x1 = cs.energy[rhs];
            double y0 = cs.cross_section[rhs - 1];
            double y1 = cs.cross_section[rhs];

            return y0 + (energy - x0) * (y1 - y0) / (x1 - x0);
        }
    }

    void sample_from_sequence(size_t n, size_t range, std::vector<size_t>& sequence, std::unordered_set<size_t>& used) {

        sequence.resize(n);
        used.clear();

        for(size_t i = 0; i < n; i++) {
            size_t num = 0;
            do {
                num = kn::random::uniform(range);
            } while(used.find(num) != used.end());
        
            used.insert(num);
            sequence[i] = num;
        }
    }

    double kinetic_energy_ev(const kn::particle::ChargedSpecies<1, 3>& p, size_t idx) {
        const auto& v = p.v()[idx];
        return 0.5 * p.m() * (v.x * v.x + v.y * v.y + v.z * v.z) / kn::constants::e;
    }

    double collision_frequency(double neutral_density, double cross_section, double kinetic_energy, double mass) {
        return neutral_density * cross_section * std::sqrt(2.0 * kn::constants::e * kinetic_energy / mass);
    }

    kn::core::Vec3 isotropic_scatter(const kn::core::Vec<3>& v, double chi) {

        const auto vn = v.normalized();
       
        const double phi = 2 * kn::constants::pi * kn::random::uniform();
        const double zeta = std::acos(vn.z);

        const double k0 = std::cos(chi);
        const double k1 = std::sin(chi) / std::sin(zeta);
        const double k2 = k1 * std::sin(phi);
        const double k3 = k1 * std::cos(phi);

        return {
            vn.x * k0 +  vn.y * k2 + vn.x * vn.z * k3, 
            vn.y * k0 -  vn.x * k2 + vn.y * vn.z * k3,
            vn.z * k0 - (vn.x * vn.x + vn.y * vn.y) * k3
        };
    }

    double electron_elastic_vmag(double kinetic_energy, double chi, double ion_mass) {
        double delta_energy = (2.0 * kn::constants::m_e / ion_mass) * (1.0 - std::cos(chi));
        return std::sqrt(2.0 * kn::constants::e * (kinetic_energy * (1.0 - delta_energy)) / kn::constants::m_e);
    }

    double electron_excitation_vmag(double kinetic_energy, double excitation_energy) {
        return std::sqrt(2.0 * kn::constants::e * (kinetic_energy - excitation_energy) / kn::constants::m_e);
    }

    double electron_ionization_vmag(double kinetic_energy, double ionization_energy) {
        // No x2 because of ionization energy division
        return std::sqrt(kn::constants::e * (kinetic_energy - ionization_energy) / kn::constants::m_e);
    }

    double random_chi() {
        return std::acos(1.0 - 2.0 * kn::random::uniform());
    }

    double random_chi2() {
        return std::acos(sqrt(1.0 - kn::random::uniform()));
    }
}

MonteCarloCollisions::MonteCarloCollisions(DomainConfig config, std::vector<CollisionReaction>&& cs) : m_config(config) {

    // TODO(lui): check how are the move assignment operators implemented by
    // default, to check if this is is being moved correctly without copy. 
    // m_el_cs = std::move(el_cs);
    // m_exc_cs = std::move(exc_cs);
    // m_iz_cs = std::move(iz_cs);
    // m_iso_cs = std::move(iso_cs);
    // m_bs_cs = std::move(bs_cs);


    for(auto& reaction : cs)  {
        if(reaction.projectile == CollisionProjectile::Ion) {
            m_ion_cs.push_back(std::move(reaction));
        } else if(reaction.projectile == CollisionProjectile::Electron) {
            m_electron_cs.push_back(std::move(reaction));
        }
    }



    // Initialize the MCC parameters
    init();
}

void MonteCarloCollisions::init() {   
    m_nu_prime_e = calc_nu_prime_electrons();
    m_p_null_e = calc_p_null(m_nu_prime_e);
    m_nu_prime_i = calc_nu_prime_ions();
    m_p_null_i = calc_p_null(m_nu_prime_i);
}

double MonteCarloCollisions::calc_p_null(double nu_prime) {
    return 1.0 - std::exp(-nu_prime * m_config.m_dt);
}

double MonteCarloCollisions::total_cs_electrons(double energy) {
    double cs = 0.0;
    for(const auto& c : m_electron_cs)
        cs += interpolate_cross_section(c, energy);
    return cs;
}

double MonteCarloCollisions::total_cs_ions(double energy) {
    double cs = 0.0;
    for(const auto& c : m_ion_cs)
        cs += interpolate_cross_section(c, energy);
    return cs;
}

double MonteCarloCollisions::nu_prime_electrons_max(const MonteCarloCollisions::CollisionReaction &cs) {

        double nu_prime = 0.0;
        const double rmc = kn::constants::e / kn::constants::m_e;

        for (size_t i = 0; i < cs.energy.size(); i++) {
            double energy = cs.energy[i];
            double tcs = total_cs_electrons(energy);
            double nu = m_config.m_n_neutral * tcs * std::sqrt(2.0 * energy * rmc);
            nu_prime = std::max(nu_prime, nu);
        }

        return nu_prime;
}

double MonteCarloCollisions::nu_prime_ions_max(const MonteCarloCollisions::CollisionReaction &cs) {

        double nu_prime = 0.0;
        const double rmc = kn::constants::e / m_config.m_m_ion;

        for (size_t i = 0; i < cs.energy.size(); i++) {
            double energy = cs.energy[i];
            double tcs = total_cs_ions(energy);
            double nu = m_config.m_n_neutral * tcs * std::sqrt(4.0 * energy * rmc);
            nu_prime = std::max(nu_prime, nu);
        }

        return nu_prime;
}

double MonteCarloCollisions::calc_nu_prime_electrons() {
    double nu_prime = 0.0;

    // TODO(lui): it's not necessary to repeat the process for all the 
    // cross sections since only the energy values are evaluated. Refactor
    // later. 
    for(const auto& cs : m_electron_cs)
        nu_prime = std::max(nu_prime, nu_prime_electrons_max(cs));

    return nu_prime;
}

double MonteCarloCollisions::calc_nu_prime_ions() {
    // TODO(lui): same as above. 
    // NOT SURE ABOUT THIS!!
    double nu_prime = 0.0;
    for(const auto& cs : m_ion_cs)
        nu_prime = std::max(nu_prime, nu_prime_ions_max(cs));
    return nu_prime;
}

double MonteCarloCollisions::frequency_ratio(const CollisionReaction& cs, double kinetic_energy) {
    return collision_frequency(m_config.m_n_neutral, interpolate_cross_section(cs, kinetic_energy), kinetic_energy, kn::constants::m_e) / m_nu_prime_e;
}

void MonteCarloCollisions::isotropic_coll(particle::ChargedSpecies<1, 3>& species, size_t idx, double vmag, double chi) {
    auto vs = isotropic_scatter(species.v()[idx], chi);
    species.v()[idx] = {vs.x * vmag, vs.y * vmag, vs.z * vmag};
}

bool MonteCarloCollisions::electron_elastic_coll(particle::ChargedSpecies<1, 3> &electrons,
                                     particle::ChargedSpecies<1, 3> &ions,
                                     size_t p_idx, double kinetic_energy) {
    double chi = random_chi();
    isotropic_coll(electrons, p_idx,electron_elastic_vmag(kinetic_energy, chi, ions.m()), chi);
    return true;
}

bool MonteCarloCollisions::electron_excitation_coll(
    particle::ChargedSpecies<1, 3> &electrons, particle::ChargedSpecies<1, 3> &ions,
    size_t p_idx, double kinetic_energy, double threshold) {
    
    if(kinetic_energy < threshold)
        return false;
    
    double chi = std::acos(1.0 - 2.0 * random::uniform());
    isotropic_coll(
        electrons, p_idx,
        electron_excitation_vmag(kinetic_energy, threshold), chi);
    return true;
}

bool MonteCarloCollisions::electron_ionization_coll(
    particle::ChargedSpecies<1, 3> &electrons, particle::ChargedSpecies<1, 3> &ions,
    size_t p_idx, double kinetic_energy, double threshold) {

    if (kinetic_energy < threshold)
        return false;

    electrons.add_copy(p_idx);
    size_t p_idx_new = electrons.n() - 1;

    auto event_pos = electrons.x()[p_idx];
    double ion_mass = ions.m();
    double neutral_temperature = m_config.m_t_neutral;
    double vmag =
        electron_ionization_vmag(kinetic_energy, threshold);

    double chi1 = std::acos(1.0 - 2.0 * random::uniform());
    isotropic_coll(electrons, p_idx, vmag, chi1);

    // Generated electron
    double chi2 = std::acos(1.0 - 2.0 * random::uniform());
    isotropic_coll(electrons, p_idx_new, vmag, chi2);

    // Generated ion
    // TODO(lui): Move from std::function to something with better performance
    ions.add(1, [event_pos, ion_mass, neutral_temperature](core::Vec<3> &v,
                                                           core::Vec<1> &x) {
      x = event_pos;
      double vtemp =
          std::sqrt(kn::constants::kb * neutral_temperature / ion_mass);
      v = {random::normal(0.0, vtemp), random::normal(0.0, vtemp),
           random::normal(0.0, vtemp)};
    });

    return true;
}

int MonteCarloCollisions::collide_electrons(
    particle::ChargedSpecies<1, 3> &electrons,
    particle::ChargedSpecies<1, 3> &ions) {

    double n_null_f = m_p_null_e * (double)electrons.n();
    size_t n_null = (size_t) std::floor(n_null_f);
    n_null = (n_null_f - (double)n_null) > random::uniform() ? n_null + 1 : n_null;

    // TODO(lui): check the performance of this sequence generation.
    sample_from_sequence(n_null, electrons.n(), m_particle_samples, m_used_cache);

    double fr0 = 0.0;
	double fr1 = 0.0;

    for(size_t i = 0; i < n_null; i++) {
        
        size_t p_idx = m_particle_samples[i];

        double kinetic_energy = kinetic_energy_ev(electrons, p_idx);
        double r1 = random::uniform();

        fr0 = 0.0;
        fr1 = 0.0;

        bool collided = false;

        for(const auto& cs: m_electron_cs) {
            fr0 = fr1;
            fr1 += frequency_ratio(cs, kinetic_energy);
            
            if(r1 > fr0 && r1 <= fr1) {
                
                switch (cs.type) {    
                    case CollisionType::Ionization: {
                        collided = electron_ionization_coll(electrons, ions, p_idx, kinetic_energy, cs.energy_threshold);
                        break;
                    }
                    case CollisionType::Excitation: {
                        collided = electron_excitation_coll(electrons, ions, p_idx, kinetic_energy, cs.energy_threshold);
                        break;
                    }
                    
                    case CollisionType::Elastic: {
                        collided = electron_elastic_coll(electrons, ions, p_idx, kinetic_energy);
                        break;
                    }

                    default:
                        collided = false;
                        break;
                }
            }

            if(collided)
                break;
        }
    }

    return 0;
}

bool MonteCarloCollisions::ions_isotropic_coll(particle::ChargedSpecies<1, 3> &ions,
                                     size_t p_idx,
                                     double kinetic_energy_rel) {
    double chi = random_chi2();
    double cos_chi = std::cos(chi);
    double vmag =
        std::sqrt(2.0 * kn::constants::e *
                  (kinetic_energy_rel * cos_chi * cos_chi) / ions.m());
    isotropic_coll(ions, p_idx, vmag, chi);

    return true;
}


void MonteCarloCollisions::collide_ions(particle::ChargedSpecies<1, 3> &ions) {
    double n_null_f = m_p_null_i * (double)ions.n();
    size_t n_null = (size_t) std::floor(n_null_f);
    n_null = (n_null_f - (double)n_null) > random::uniform() ? n_null + 1 : n_null;

    // TODO(lui): check the performance of this sequence generation.
    sample_from_sequence(n_null, ions.n(), m_particle_samples, m_used_cache);

    double vth = std::sqrt(kn::constants::kb * m_config.m_t_neutral / ions.m());

    double fr0 = 0.0;
	double fr1 = 0.0;

    for(size_t i = 0; i < n_null; i++) {

        size_t p_idx = m_particle_samples[i];

        core::Vec3 v_rand_neutral = {
            kn::random::normal() * vth,
            kn::random::normal() * vth,
            kn::random::normal() * vth
        };

        auto& vp = ions.v()[p_idx];
        vp.x -= v_rand_neutral.x;
        vp.y -= v_rand_neutral.y;
        vp.z -= v_rand_neutral.z;

        double kinetic_energy_rel = kinetic_energy_ev(ions, p_idx);

        double r1 = kn::random::uniform();


        fr0 = 0.0;
        fr1 = 0.0;

        bool collided = false;

        for(const auto& cs : m_ion_cs) {
            fr0 = fr1;
            fr1 += collision_frequency(m_config.m_n_neutral, interpolate_cross_section(cs, 0.5 * kinetic_energy_rel),  kinetic_energy_rel, ions.m()) / m_nu_prime_i;

            if(r1 > fr0 && r1 <= fr1) {
                
                switch (cs.type) {    
                    case CollisionType::Isotropic: {
                        ions_isotropic_coll(ions, p_idx, kinetic_energy_rel);
                        break;
                    }
                    case CollisionType::Backscattering: {
                        vp = core::Vec<3>();
                        collided = true;
                        break;
                    }
                    
                    default:
                        break;
                }
            }

            if(collided)
                break;

        }
     
        vp.x += v_rand_neutral.x;
        vp.y += v_rand_neutral.y;
        vp.z += v_rand_neutral.z;
    }
}
