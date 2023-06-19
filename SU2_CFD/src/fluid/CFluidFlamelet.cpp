/*!
 * \file CfluidFlamelet.cpp
 * \brief Main subroutines of CFluidFlamelet class
 * \author D. Mayer, T. Economon, N. Beishuizen, E. Bunschoten
 * \version 7.5.1 "Blackbird"
 *
 * SU2 Project Website: https://su2code.github.io
 *
 * The SU2 Project is maintained by the SU2 Foundation
 * (http://su2foundation.org)
 *
 * Copyright 2012-2023, SU2 Contributors (cf. AUTHORS.md)
 *
 * SU2 is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * SU2 is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with SU2. If not, see <http://www.gnu.org/licenses/>.
 */

#include "../include/fluid/CFluidFlamelet.hpp"
#include "../../../Common/include/containers/CLookUpTable.hpp"

CFluidFlamelet::CFluidFlamelet(CConfig* config, su2double value_pressure_operating, bool load_manifold) : CFluidModel(), generate_manifold(load_manifold) {
  rank = SU2_MPI::GetRank();

  /* -- number of auxiliary species transport equations, e.g. 1=CO, 2=NOx  --- */
  n_user_scalars = config->GetNUserScalars();
  n_control_vars = config->GetNControlVars();
  include_mixture_fraction = (n_control_vars == 3);
  n_scalars = config->GetNScalars();

  if (rank == MASTER_NODE) {
    cout << "Number of scalars:           " << n_scalars << endl;
    cout << "Number of user scalars:      " << n_user_scalars << endl;
    cout << "Number of control variables: " << n_control_vars << endl;
  }

  if (rank == MASTER_NODE) {
    cout << "*****************************************" << endl;
    cout << "***   initializing the lookup table   ***" << endl;
    cout << "*****************************************" << endl;
  }

  scalars_vector.resize(n_scalars);

  table_scalar_names.resize(n_scalars);
  table_scalar_names[I_ENTH] = "EnthalpyTot";
  table_scalar_names[I_PROGVAR] = "ProgressVariable";
  if (include_mixture_fraction) table_scalar_names[I_MIXFRAC] = "MixtureFraction";

  /*--- auxiliary species transport equations---*/
  for (size_t i_aux = 0; i_aux < n_user_scalars; i_aux++) {
    table_scalar_names[n_control_vars + i_aux] = config->GetUserScalarName(i_aux);
  }
  
  if (generate_manifold)
    look_up_table = new CLookUpTable(config->GetFileNameLUT(), table_scalar_names[I_PROGVAR], table_scalar_names[I_ENTH]);

  Pressure = value_pressure_operating;

  PreprocessLookUp(config);
 
}

CFluidFlamelet::~CFluidFlamelet() { if(generate_manifold) delete look_up_table; }

void CFluidFlamelet::SetTDState_T(su2double val_temperature, const su2double* val_scalars) {
  for (auto iVar = 0u; iVar < n_scalars; iVar++) scalars_vector[iVar] = val_scalars[iVar];

  /*--- Add all quantities and their names to the look up vectors. ---*/
  EvaluateDataSet(scalars_vector, FLAMELET_LOOKUP_OPS::TD, val_vars_TD);

  Temperature = val_vars_TD[LOOKUP_TD::TEMPERATURE];
  Cp = val_vars_TD[LOOKUP_TD::HEATCAPACITY];
  Mu = val_vars_TD[LOOKUP_TD::VISCOSITY];
  Kt = val_vars_TD[LOOKUP_TD::CONDUCTIVITY];
  mass_diffusivity = val_vars_TD[LOOKUP_TD::DIFFUSIONCOEFFICIENT];
  molar_weight = val_vars_TD[LOOKUP_TD::MOLARWEIGHT];
  Density = Pressure / (molar_weight * UNIVERSAL_GAS_CONSTANT * Temperature);

  /*--- Compute Cv from Cp and molar weight of the mixture (ideal gas). ---*/
  Cv = Cp - UNIVERSAL_GAS_CONSTANT / molar_weight;
}

void CFluidFlamelet::PreprocessLookUp(CConfig* config) {
   /*--- Thermodynamic state variables and names. ---*/
  varnames_TD.resize(LOOKUP_TD::SIZE);
  val_vars_TD.resize(LOOKUP_TD::SIZE);

  /*--- The string in varnames_TD as it appears in the LUT file. ---*/
  varnames_TD[LOOKUP_TD::TEMPERATURE] = "Temperature";
  varnames_TD[LOOKUP_TD::HEATCAPACITY] = "Cp";
  varnames_TD[LOOKUP_TD::VISCOSITY] = "ViscosityDyn";
  varnames_TD[LOOKUP_TD::CONDUCTIVITY] = "Conductivity";
  varnames_TD[LOOKUP_TD::DIFFUSIONCOEFFICIENT] = "DiffusionCoefficient";
  varnames_TD[LOOKUP_TD::MOLARWEIGHT] = "MolarWeightMix";

  /*--- Scalar source term variables and names. ---*/
  size_t n_sources = 1 + 2*n_user_scalars;
  varnames_Sources.resize(n_sources);
  val_vars_Sources.resize(n_sources);
  varnames_Sources[I_SRC_TOT_PROGVAR] = "ProdRateTot_PV";
  /*--- No source term for enthalpy ---*/

  /*--- For the auxiliary equations, we use a positive (production) and a negative (consumption) term:
        S_tot = S_PROD + S_CONS * Y ---*/

  for (size_t i_aux = 0; i_aux < n_user_scalars; i_aux++) {
    /*--- Order of the source terms: S_prod_1, S_cons_1, S_prod_2, S_cons_2, ...---*/
    varnames_Sources[1 + 2 * i_aux] = config->GetUserSourceName(2 * i_aux);
    varnames_Sources[1 + 2 * i_aux + 1] = config->GetUserSourceName(2 * i_aux + 1);
  }

  /*--- Passive look-up terms ---*/
  size_t n_lookups = config->GetNLookups();
  varnames_LookUp.resize(n_lookups);
  val_vars_LookUp.resize(n_lookups);
  for (auto iLookup=0u; iLookup < n_lookups; iLookup++)
    varnames_LookUp[iLookup] = config->GetLUTLookupName(iLookup);

}

unsigned long CFluidFlamelet::EvaluateDataSet(vector<su2double> &input_scalar, FLAMELET_LOOKUP_OPS lookup_type, vector<su2double> &output_refs) {
  su2double val_enth = input_scalar[I_ENTH];
  su2double val_prog = input_scalar[I_PROGVAR];
  su2double val_mixfrac = include_mixture_fraction ? input_scalar[I_MIXFRAC] : 0.0;
  vector<string> varnames;
  vector<su2double> val_vars;
  switch (lookup_type)
  {
  case FLAMELET_LOOKUP_OPS::TD:
    varnames = varnames_TD;
    break;
  case FLAMELET_LOOKUP_OPS::SOURCES:
    varnames = varnames_Sources;
    break;
  case FLAMELET_LOOKUP_OPS::LOOKUP:
    varnames = varnames_LookUp;
    break;
  
  default:
    break;
  }
  if (output_refs.size() != varnames.size())
    SU2_MPI::Error(string("Output vector size incompatible with manifold lookup operation."), CURRENT_FUNCTION);

  /*--- Add all quantities and their names to the look up vectors. ---*/
  if (include_mixture_fraction) {
    extrapolation = look_up_table->LookUp_XYZ(varnames, output_refs, val_prog, val_enth, val_mixfrac);
  } else {
    extrapolation = look_up_table->LookUp_XY(varnames, output_refs, val_prog, val_enth);
  }

  return extrapolation;
}
