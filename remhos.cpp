// Copyright (c) 2017, Lawrence Livermore National Security, LLC. Produced at
// the Lawrence Livermore National Laboratory. LLNL-CODE-734707. All Rights
// reserved. See files LICENSE and NOTICE for details.
//
// This file is part of CEED, a collection of benchmarks, miniapps, software
// libraries and APIs for efficient high-order finite element and spectral
// element discretizations for exascale applications. For more information and
// source code availability see http://github.com/ceed.
//
// The CEED research is supported by the Exascale Computing Project 17-SC-20-SC,
// a collaborative effort of two U.S. Department of Energy organizations (Office
// of Science and the National Nuclear Security Administration) responsible for
// the planning and preparation of a capable exascale ecosystem, including
// software, applications, hardware, advanced system engineering and early
// testbed platforms, in support of the nation's exascale computing imperative.
//
//                    ____                 __
//                   / __ \___  ____ ___  / /_  ____  _____
//                  / /_/ / _ \/ __ `__ \/ __ \/ __ \/ ___/
//                 / _, _/  __/ / / / / / / / / /_/ (__  )
//                /_/ |_|\___/_/ /_/ /_/_/ /_/\____/____/
//
//                       High-order Remap Miniapp
//
// Remhos (REMap High-Order Solver) is a miniapp that solves the pure advection
// equations that are used to perform discontinuous field interpolation (remap)
// as part of the Eulerian phase in Arbitrary-Lagrangian Eulerian (ALE)
// simulations.
//
// Sample runs: see README.md, section 'Verification of Results'.

#define MFEM_DEBUG_COLOR 154
#include "debug.hpp"

#include "mfem.hpp"
#include <fstream>
#include <iostream>
#include "remhos.hpp"
#include "remhos_ho.hpp"
#include "remhos_lo.hpp"
#include "remhos_ibc.hpp"
#include "remhos_fct.hpp"
#include "remhos_mono.hpp"
#include "remhos_tools.hpp"
#include "remhos_sync.hpp"
#include "remhos_adv.hpp"
#include "remhos_amr.hpp"


using namespace std;
using namespace mfem;

// Visualization window position and size
const int Wx = 800, Wy = 300;
const int Ww = 640, Wh = 640;

// Mesh bounding box
Vector bb_min, bb_max;

/// Main ///
int main(int argc, char *argv[])
{
   // Initialize MPI.
   MPI_Session mpi(argc, argv);
   const int myid = mpi.WorldRank();

   const char *mesh_file = "data/periodic-square.mesh";
   int rs_levels = 2;
   int rp_levels = 0;
   int order = 3;
   int mesh_order = 2;
   int ode_solver_type = 3;
   HOSolverType ho_type           = HOSolverType::LocalInverse;
   LOSolverType lo_type           = LOSolverType::None;
   FCTSolverType fct_type         = FCTSolverType::None;
   MonolithicSolverType mono_type = MonolithicSolverType::None;
   int bounds_type = 0;
   bool pa = false;
   bool next_gen_full = false;
   int smth_ind_type = 0;
   double t_final = 4.0;
   TimeStepControl dt_control = TimeStepControl::FixedTimeStep;
   double dt = 0.005;
   int max_tsteps = -1;
   bool visualization = true;
   bool visit = false;
   bool verify_bounds = false;
   bool product_sync = false;
   int vis_steps = 100;
   const char *device_config = "cpu";
   bool amr = false;
   int amr_estimator = amr::Estimator::L2ZZ;
   double amr_ref_threshold = 2e-3;
   double amr_deref_threshold = 1e-5;
   int amr_max_level = 2; // only for JJt
   int amr_nc_limit = 0;
   double dt_factor = 2.0;

   int precision = 8;
   cout.precision(precision);

   OptionsParser args(argc, argv);
   args.AddOption(&mesh_file, "-m", "--mesh",
                  "Mesh file to use.");
   args.AddOption(&problem_num, "-p", "--problem",
                  "Problem setup to use. See options in velocity_function().");
   args.AddOption(&rs_levels, "-rs", "--refine-serial",
                  "Number of times to refine the mesh uniformly in serial.");
   args.AddOption(&rp_levels, "-rp", "--refine-parallel",
                  "Number of times to refine the mesh uniformly in parallel.");
   args.AddOption(&order, "-o", "--order",
                  "Order (degree) of the finite element solution.");
   args.AddOption(&mesh_order, "-mo", "--mesh-order",
                  "Order (degree) of the mesh.");
   args.AddOption(&ode_solver_type, "-s", "--ode-solver",
                  "ODE solver: 1 - Forward Euler,\n\t"
                  "            2 - RK2 SSP, 3 - RK3 SSP, 4 - RK4, 6 - RK6.");
   args.AddOption((int*)(&ho_type), "-ho", "--ho-type",
                  "High-Order Solver: 0 - No HO solver,\n\t"
                  "                   1 - Neumann iteration,\n\t"
                  "                   2 - CG solver,\n\t"
                  "                   3 - Local inverse.");
   args.AddOption((int*)(&lo_type), "-lo", "--lo-type",
                  "Low-Order Solver: 0 - No LO solver,\n\t"
                  "                  1 - Discrete Upwind,\n\t"
                  "                  2 - Preconditioned Discrete Upwind,\n\t"
                  "                  3 - Residual Distribution,\n\t"
                  "                  4 - Subcell Residual Distribution,\n\t"
                  "                  5 - Mass-Based Element Average.");
   args.AddOption((int*)(&fct_type), "-fct", "--fct-type",
                  "Correction type: 0 - No nonlinear correction,\n\t"
                  "                 1 - Flux-based FCT,\n\t"
                  "                 2 - Local clip + scale,\n\t"
                  "                 3 - Local clip + nonlinear penalization.");
   args.AddOption((int*)(&mono_type), "-mono", "--mono-type",
                  "Monolithic solver: 0 - No monolithic solver,\n\t"
                  "                   1 - Residual Distribution,\n\t"
                  "                   2 - Subcell Residual Distribution.");
   args.AddOption(&bounds_type, "-bt", "--bounds-type",
                  "Bounds stencil type: 0 - overlapping elements,\n\t"
                  "                     1 - matrix sparsity pattern.");
   args.AddOption(&pa, "-pa", "--partial-assembly", "-no-pa",
                  "--no-partial-assembly",
                  "Enable or disable partial assembly for the HO solution.");
   args.AddOption(&next_gen_full, "-full", "--next-gen-full", "-no-full",
                  "--no-next-gen-full",
                  "Enable or disable next gen full assembly for the HO solution.");
   args.AddOption(&device_config, "-d", "--device",
                  "Device configuration string, see Device::Configure().");
   args.AddOption(&smth_ind_type, "-si", "--smth_ind",
                  "Smoothness indicator: 0 - no smoothness indicator,\n\t"
                  "                      1 - approx_quadratic,\n\t"
                  "                      2 - exact_quadratic.");
   args.AddOption(&t_final, "-tf", "--t-final",
                  "Final time; start time is 0.");
   args.AddOption((int*)(&dt_control), "-dtc", "--dt-control",
                  "Time Step Control: 0 - Fixed time step, set with -dt,\n\t"
                  "                   1 - Bounds violation of the LO sltn.");
   args.AddOption(&dt, "-dt", "--time-step",
                  "Initial time step size (dt might change based on -dtc).");
   args.AddOption(&visualization, "-vis", "--visualization", "-no-vis",
                  "--no-visualization",
                  "Enable or disable GLVis visualization.");
   args.AddOption(&visit, "-visit", "--visit-datafiles", "-no-visit",
                  "--no-visit-datafiles",
                  "Save data files for VisIt (visit.llnl.gov) visualization.");
   args.AddOption(&verify_bounds, "-vb", "--verify-bounds", "-no-vb",
                  "--no-verify-bounds",
                  "Verify solution bounds after each time step.");
   args.AddOption(&product_sync, "-ps", "--product-sync", "-no-ps",
                  "--no-product-sync",
                  "Enable remap of synchronized product fields.");
   args.AddOption(&vis_steps, "-vs", "--visualization-steps",
                  "Visualize every n-th timestep.");
   args.AddOption(&amr, "-amr", "--enable-amr", "-no-amr", "--disable-amr",
                  "Enable adaptive mesh refinement.");
   args.AddOption(&amr_estimator, "-ae", "--amr-estimator",
                  "AMR estimator: 0:ZZ, 1:L2ZZ, 2:JJt, 3:Custom");
   args.AddOption(&amr_ref_threshold, "-ar", "--amr-ref-threshold",
                  "AMR refinement threshold.");
   args.AddOption(&amr_deref_threshold, "-ad", "--amr-deref-threshold",
                  "AMR refinement threshold.");
   args.AddOption(&amr_max_level, "-am", "--amr-max-level",
                  "AMR max refined level "
                  "(after the initial serial and parallel refinements)");
   args.AddOption(&amr_nc_limit, "-al", "--amr-nc-limit",
                  "AMR maximum level of hanging nodes, (0 = unlimited)");
   args.AddOption(&dt_factor, "-df", "--time-step-factor",
                  "Time step factor when adding an AMR depth level.");
   args.AddOption(&max_tsteps, "-ms", "--max-steps",
                  "Maximum number of steps (negative means no restriction).");

   args.Parse();
   if (!args.Good())
   {
      if (myid == 0) { args.PrintUsage(cout); }
      return 1;
   }
   if (myid == 0) { args.PrintOptions(cout); }

   // Enable hardware devices such as GPUs, and programming models such as
   // CUDA, OCCA, RAJA and OpenMP based on command line options.
   Device device(device_config);
   if (myid == 0) { device.Print(); }

   // When not using lua, exec mode is derived from problem number convention
   if (problem_num < 10)      { exec_mode = 0; }
   else if (problem_num < 20) { exec_mode = 1; }
   else { MFEM_ABORT("Unspecified execution mode."); }

   // AMR sanity checks
   // Re-evaluate amr_max_level after arguments have been parsed
   if (amr)
   {
      MFEM_VERIFY(exec_mode == 0, "Only standard transport is supported.")
   }

   // Read the serial mesh from the given mesh file on all processors.
   // Refine the mesh in serial to increase the resolution.
   Mesh *mesh = new Mesh(Mesh::LoadFromFile(mesh_file, 1, 1));
   const int dim = mesh->Dimension();
   if (amr)
   {
      mesh->EnsureNCMesh();
      amr_max_level += rs_levels + rp_levels;
   }

   const amr::Options amr_options = { amr_estimator,
                                      order, mesh_order,
                                      amr_max_level, amr_nc_limit,
                                      amr_ref_threshold, amr_deref_threshold
                                    };

   for (int lev = 0; lev < rs_levels; lev++) { mesh->UniformRefinement(); }
   mesh->GetBoundingBox(bb_min, bb_max, max(order, 1));
   mesh->Finalize(true);

   // Only standard assembly in 1D (some mfem functions just abort in 1D).
   if ((pa || next_gen_full) && dim == 1)
   {
      MFEM_WARNING("Disabling PA / FA for 1D.");
      pa = false;
      next_gen_full = false;
   }

   // Parallel partitioning of the mesh.
   // Refine the mesh further in parallel to increase the resolution.
   ParMesh pmesh(MPI_COMM_WORLD, *mesh);
   delete mesh;
   for (int lev = 0; lev < rp_levels; lev++) { pmesh.UniformRefinement(); }
   MPI_Comm comm = pmesh.GetComm();
   const int NE  = pmesh.GetNE();

   // Define the ODE solver used for time integration. Several explicit
   // Runge-Kutta methods are available.
   ODESolver *ode_solver = NULL;
   switch (ode_solver_type)
   {
      case 1: ode_solver = new ForwardEulerSolver; break;
      case 2: ode_solver = new RK2Solver(1.0); break;
      case 3: ode_solver = new RK3SSPSolver; break;
      case 4:
         if (myid == 0) { MFEM_WARNING("RK4 may violate the bounds."); }
         ode_solver = new RK4Solver; break;
      case 6:
         if (myid == 0) { MFEM_WARNING("RK6 may violate the bounds."); }
         ode_solver = new RK6Solver; break;
      default:
         cout << "Unknown ODE solver type: " << ode_solver_type << '\n';
         return 3;
   }

   // Check if the input mesh is periodic.
   const bool periodic = pmesh.GetNodes() != NULL &&
                         dynamic_cast<const L2_FECollection *>
                         (pmesh.GetNodes()->FESpace()->FEColl()) != NULL;
   pmesh.SetCurvature(mesh_order, periodic);

   if (amr) { pmesh.EnsureNCMesh(); }

   FiniteElementCollection *mesh_fec;
   if (periodic)
   {
      dbg("\033[7mPeriodic => L2_FECollection");
      mesh_fec = new L2_FECollection(mesh_order, dim, BasisType::GaussLobatto);
   }
   else
   {
      dbg("\033[7mStandard => H1_FECollection");
      mesh_fec = new H1_FECollection(mesh_order, dim, BasisType::GaussLobatto);
   }
   // Current mesh positions.
   ParFiniteElementSpace mesh_pfes(&pmesh, mesh_fec, dim);
   ParGridFunction x(&mesh_pfes);
   pmesh.SetNodalGridFunction(&x);

   // Store initial mesh positions.
   Vector x0(x.Size());
   x0 = x;

   // Velocity for the problem. Depending on the execution mode, this is the
   // advective velocity (transport) or mesh velocity (remap).
   VectorFunctionCoefficient velocity(dim, velocity_function);

   // Initial time step estimate (CFL-based).
   if (dt < 0.0)
   {
      dt = std::numeric_limits<double>::infinity();
      Vector vel_e(dim);
      for (int e = 0; e < NE; e++)
      {
         double length_e = pmesh.GetElementSize(e);
         auto Tr = pmesh.GetElementTransformation(e);
         auto ip = Geometries.GetCenter(pmesh.GetElementBaseGeometry(e));
         Tr->SetIntPoint(&ip);
         velocity.Eval(vel_e, *Tr, ip);
         double speed_e = sqrt(vel_e * vel_e + 1e-14);
         dt = fmin(dt, 0.25 * length_e / speed_e);
      }
      MPI_Allreduce(MPI_IN_PLACE, &dt, 1, MPI_DOUBLE, MPI_MIN, comm);
   }

   // Mesh velocity.
   // If remap is on, obtain the mesh velocity by moving the mesh to the final
   // mesh positions, and taking the displacement vector.
   // The mesh motion resembles a time-dependent deformation, e.g., similar to
   // a deformation that is obtained by a Lagrangian simulation.
   GridFunction v_gf(x.FESpace());
   VectorGridFunctionCoefficient v_mesh_coeff(&v_gf);
   if (exec_mode == 1)
   {
      ParGridFunction v(&mesh_pfes);
      VectorFunctionCoefficient vcoeff(dim, velocity_function);
      v.ProjectCoefficient(vcoeff);

      double t = 0.0;
      while (t < t_final)
      {
         t += dt;
         // Move the mesh nodes.
         x.Add(std::min(dt, t_final-t), v);
         // Update the node velocities.
         v.ProjectCoefficient(vcoeff);
      }

      // Pseudotime velocity.
      add(x, -1.0, x0, v_gf);

      // Return the mesh to the initial configuration.
      x = x0;
   }

   // Define the discontinuous DG finite element space of the given
   // polynomial order on the refined mesh.
   const int btype = BasisType::Positive;
   DG_FECollection fec(order, dim, btype);
   ParFiniteElementSpace pfes(&pmesh, &fec);

   // Check for meaningful combinations of parameters.
   const bool forced_bounds = lo_type   != LOSolverType::None ||
                              mono_type != MonolithicSolverType::None;
   if (forced_bounds)
   {
      MFEM_VERIFY(btype == 2,
                  "Monotonicity treatment requires Bernstein basis.");

      if (order == 0)
      {
         // Disable monotonicity treatment for piecewise constants.
         if (myid == 0)
         { mfem_warning("For -o 0, monotonicity treatment is disabled."); }
         lo_type = LOSolverType::None;
         fct_type = FCTSolverType::None;
         mono_type = MonolithicSolverType::None;
      }
   }
   const bool use_subcell_RD =
      ( lo_type   == LOSolverType::ResDistSubcell ||
        mono_type == MonolithicSolverType::ResDistMonoSubcell );
   if (use_subcell_RD)
   {
      MFEM_VERIFY(order > 1, "Subcell schemes require FE order > 2.");
   }
   if (dt_control == TimeStepControl::LOBoundsError)
   {
      MFEM_VERIFY(bounds_type == 1, "Error: -dtc 1 requires -bt 1.");
   }

   const int prob_size = pfes.GlobalTrueVSize();
   if (myid == 0) { cout << "Number of unknowns: " << prob_size << endl; }

   // Fields related to inflow BC.
   FunctionCoefficient inflow(inflow_function);
   ParGridFunction inflow_gf(&pfes);
   if (problem_num == 7) // Convergence test: use high order projection.
   {
      L2_FECollection l2_fec(order, dim);
      ParFiniteElementSpace l2_fes(&pmesh, &l2_fec);
      ParGridFunction l2_inflow(&l2_fes);
      l2_inflow.ProjectCoefficient(inflow);
      inflow_gf.ProjectGridFunction(l2_inflow);
   }
   else { inflow_gf.ProjectCoefficient(inflow); }

   // Set up the bilinear and linear forms corresponding to the DG
   // discretization.
   ParBilinearForm m(&pfes);
   m.AddDomainIntegrator(new MassIntegrator);

   ParBilinearForm M_HO(&pfes);
   M_HO.AddDomainIntegrator(new MassIntegrator);

   ParBilinearForm k(&pfes);
   ParBilinearForm K_HO(&pfes);
   if (exec_mode == 0)
   {
      k.AddDomainIntegrator(new ConvectionIntegrator(velocity, -1.0));
      K_HO.AddDomainIntegrator(new ConvectionIntegrator(velocity, -1.0));
   }
   else if (exec_mode == 1)
   {
      k.AddDomainIntegrator(new ConvectionIntegrator(v_mesh_coeff));
      K_HO.AddDomainIntegrator(new ConvectionIntegrator(v_mesh_coeff));
   }

   if (ho_type == HOSolverType::CG ||
       ho_type == HOSolverType::LocalInverse ||
       fct_type == FCTSolverType::FluxBased)
   {
      if (exec_mode == 0)
      {
         DGTraceIntegrator *dgt_i = new DGTraceIntegrator(velocity, 1.0, -0.5);
         DGTraceIntegrator *dgt_b = new DGTraceIntegrator(velocity, 1.0, -0.5);
         K_HO.AddInteriorFaceIntegrator(new TransposeIntegrator(dgt_i));
         K_HO.AddBdrFaceIntegrator(new TransposeIntegrator(dgt_b));
      }
      else if (exec_mode == 1)
      {
         auto dgt_i = new DGTraceIntegrator(v_mesh_coeff, -1.0, -0.5);
         auto dgt_b = new DGTraceIntegrator(v_mesh_coeff, -1.0, -0.5);
         K_HO.AddInteriorFaceIntegrator(new TransposeIntegrator(dgt_i));
         K_HO.AddBdrFaceIntegrator(new TransposeIntegrator(dgt_b));
      }

      K_HO.KeepNbrBlock(true);
   }

   if (pa)
   {
      M_HO.SetAssemblyLevel(AssemblyLevel::PARTIAL);
      K_HO.SetAssemblyLevel(AssemblyLevel::PARTIAL);
   }

   if (next_gen_full)
   {
      K_HO.SetAssemblyLevel(AssemblyLevel::FULL);
   }

   M_HO.Assemble();
   K_HO.Assemble(0);

   if (pa == false)
   {
      M_HO.Finalize();
      K_HO.Finalize(0);
   }

   // Compute the lumped mass matrix.
   Vector lumpedM;
   ParBilinearForm ml(&pfes);
   ml.AddDomainIntegrator(new LumpedIntegrator(new MassIntegrator));
   ml.Assemble();
   ml.Finalize();
   ml.SpMat().GetDiag(lumpedM);

   m.Assemble();
   m.Finalize();
   int skip_zeros = 0;
   k.Assemble(skip_zeros);
   k.Finalize(skip_zeros);

   // Store topological dof data.
   DofInfo dofs(pfes, bounds_type);

   // Precompute data required for high and low order schemes. This could be put
   // into a separate routine. I am using a struct now because the various
   // schemes require quite different information.
   LowOrderMethod lom;
   lom.subcell_scheme = use_subcell_RD;

   lom.pk = NULL;
   if (lo_type == LOSolverType::DiscrUpwind)
   {
      lom.smap = SparseMatrix_Build_smap(k.SpMat());
      lom.D = k.SpMat();

      if (exec_mode == 0)
      {
         ComputeDiscreteUpwindingMatrix(k.SpMat(), lom.smap, lom.D);
      }
   }
   else if (lo_type == LOSolverType::DiscrUpwindPrec)
   {
      lom.pk = new ParBilinearForm(&pfes);
      if (exec_mode == 0)
      {
         lom.pk->AddDomainIntegrator(
            new PrecondConvectionIntegrator(velocity, -1.0) );
      }
      else if (exec_mode == 1)
      {
         lom.pk->AddDomainIntegrator(
            new PrecondConvectionIntegrator(v_mesh_coeff) );
      }
      lom.pk->Assemble(skip_zeros);
      lom.pk->Finalize(skip_zeros);

      lom.smap = SparseMatrix_Build_smap(lom.pk->SpMat());
      lom.D = lom.pk->SpMat();

      if (exec_mode == 0)
      {
         ComputeDiscreteUpwindingMatrix(lom.pk->SpMat(), lom.smap, lom.D);
      }
   }
   if (exec_mode == 1) { lom.coef = &v_mesh_coeff; }
   else                { lom.coef = &velocity; }

   // Face integration rule.
   const FaceElementTransformations *ft =
      pmesh.GetFaceElementTransformations(0);
   const int el_order = pfes.GetFE(0)->GetOrder();
   int ft_order = ft->Elem1->OrderW() + 2 * el_order;
   if (pfes.GetFE(0)->Space() == FunctionSpace::Pk) { ft_order++; }
   lom.irF = &IntRules.Get(ft->FaceGeom, ft_order);

   DG_FECollection fec0(0, dim, btype);
   DG_FECollection fec1(1, dim, btype);

   ParMesh *subcell_mesh = NULL;
   lom.SubFes0 = NULL;
   lom.SubFes1 = NULL;
   FiniteElementCollection *fec_sub = NULL;
   ParFiniteElementSpace *pfes_sub = NULL;;
   ParGridFunction *xsub = NULL;
   ParGridFunction v_sub_gf;
   VectorGridFunctionCoefficient v_sub_coef;
   Vector x0_sub;

   if (order > 1)
   {
      // The mesh corresponding to Bezier subcells of order p is constructed.
      // NOTE: The mesh is assumed to consist of quads or hexes.
      MFEM_VERIFY(order > 1, "This code should not be entered for order = 1.");

      // Get a uniformly refined mesh.
      const int btype = BasisType::ClosedUniform;
      subcell_mesh = new ParMesh(ParMesh::MakeRefined(pmesh, order, btype));

      // Check if the mesh is periodic.
      const L2_FECollection *L2_coll = dynamic_cast<const L2_FECollection *>
                                       (pmesh.GetNodes()->FESpace()->FEColl());
      if (L2_coll == NULL)
      {
         // Standard non-periodic mesh.
         // Note that the fine mesh is always linear.
         fec_sub = new H1_FECollection(1, dim, BasisType::ClosedUniform);
         pfes_sub = new ParFiniteElementSpace(subcell_mesh, fec_sub, dim);
         xsub = new ParGridFunction(pfes_sub);
         subcell_mesh->SetCurvature(1);
         subcell_mesh->SetNodalGridFunction(xsub);
      }
      else
      {
         // Periodic mesh - the node positions must be corrected after the call
         // to the above Mesh constructor. Note that the fine mesh is always
         // linear.
         const bool disc_nodes = true;
         subcell_mesh->SetCurvature(1, disc_nodes);

         fec_sub = new L2_FECollection(1, dim, BasisType::ClosedUniform);
         pfes_sub = new ParFiniteElementSpace(subcell_mesh, fec_sub, dim);
         xsub = new ParGridFunction(pfes_sub);
         subcell_mesh->SetNodalGridFunction(xsub);

         GridFunction *coarse = pmesh.GetNodes();
         InterpolationGridTransfer transf(*coarse->FESpace(), *pfes_sub);
         transf.ForwardOperator().Mult(*coarse, *xsub);
      }

      lom.SubFes0 = new FiniteElementSpace(subcell_mesh, &fec0);
      lom.SubFes1 = new FiniteElementSpace(subcell_mesh, &fec1);

      // Submesh velocity.
      v_sub_gf.SetSpace(pfes_sub);
      v_sub_gf.ProjectCoefficient(velocity);

      // Zero it out on boundaries (not moving boundaries).
      ZeroItOutOnBoundaries(subcell_mesh, xsub, v_sub_gf, v_sub_coef);

      // Store initial submesh positions.
      x0_sub = *xsub;

      // Integrator on the submesh.
      if (exec_mode == 0)
      {
         lom.subcellCoeff = &velocity;
         lom.VolumeTerms = new MixedConvectionIntegrator(velocity, -1.0);
      }
      else if (exec_mode == 1)
      {
         lom.subcellCoeff = &v_sub_coef;
         lom.VolumeTerms = new MixedConvectionIntegrator(v_sub_coef);
      }
   }
   else { subcell_mesh = &pmesh; }

   Assembly asmbl(dofs, lom, inflow_gf, pfes, subcell_mesh, exec_mode);

   // Setup the initial conditions.
   const int vsize = pfes.GetVSize();
   Array<int> offset((product_sync) ? 3 : 2);
   for (int i = 0; i < offset.Size(); i++) { offset[i] = i*vsize; }
   BlockVector S(offset, Device::GetMemoryType());
   // Primary scalar field is u.
   ParGridFunction u(&pfes);
   u.MakeRef(&pfes, S, offset[0]);
   FunctionCoefficient u0(u0_function);
   u.ProjectCoefficient(u0);
   u.SyncAliasMemory(S);
   /*{
      Vector tmp;
      u.GetTrueDofs(tmp);
      u.SetFromTrueDofs(tmp);
      dbg("pmesh.GetNE: %d, u.Size: %d", pmesh.GetNE(), u.Size());
   }*/
   // For the case of product remap, we also solve for s and u_s.
   ParGridFunction s, us;
   Array<bool> u_bool_el, u_bool_dofs;
   if (product_sync)
   {
      s.SetSpace(&pfes);
      ComputeBoolIndicators(pmesh.GetNE(), u, u_bool_el, u_bool_dofs);
      BoolFunctionCoefficient sc(s0_function, u_bool_el);
      s.ProjectCoefficient(sc);

      us.MakeRef(&pfes, S, offset[1]);
      double *h_us = us.HostWrite();
      const double *h_u = u.HostRead();
      const double *h_s = s.HostRead();
      // Simple - we don't target conservation at initialization.
      for (int i = 0; i < s.Size(); i++) { h_us[i] = h_u[i] * h_s[i]; }
      us.SyncAliasMemory(S);
   }

   // Smoothness indicator.
   SmoothnessIndicator *smth_indicator = NULL;
   if (smth_ind_type)
   {
      smth_indicator = new SmoothnessIndicator(smth_ind_type, *subcell_mesh,
                                               pfes, u, dofs);
   }

   // Setup of the high-order solver (if any).
   HOSolver *ho_solver = NULL;
   if (ho_type == HOSolverType::Neumann)
   {
      ho_solver = new NeumannHOSolver(pfes, m, k, lumpedM, asmbl);
   }
   else if (ho_type == HOSolverType::CG)
   {
      ho_solver = new CGHOSolver(pfes, M_HO, K_HO);
   }
   else if (ho_type == HOSolverType::LocalInverse)
   {
      ho_solver = new LocalInverseHOSolver(pfes, M_HO, K_HO);
   }

   // Setup the low order solver (if any).
   LOSolver *lo_solver = NULL;
   Array<int> lo_smap;
   const bool time_dep = (exec_mode == 0) ? false : true;
   if (lo_type == LOSolverType::DiscrUpwind)
   {
      lo_smap = SparseMatrix_Build_smap(k.SpMat());
      lo_solver = new DiscreteUpwind(pfes, k.SpMat(), lo_smap,
                                     lumpedM, asmbl, time_dep);
   }
   else if (lo_type == LOSolverType::DiscrUpwindPrec)
   {
      lo_smap = SparseMatrix_Build_smap(lom.pk->SpMat());
      lo_solver = new DiscreteUpwind(pfes, lom.pk->SpMat(), lo_smap,
                                     lumpedM, asmbl, time_dep);
   }
   else if (lo_type == LOSolverType::ResDist)
   {
      const bool subcell_scheme = false;
      if (pa)
      {
         lo_solver = new PAResidualDistribution(pfes, k, asmbl, lumpedM,
                                                subcell_scheme, time_dep);
         if (exec_mode == 0)
         {
            const PAResidualDistribution *RD_ptr =
               dynamic_cast<const PAResidualDistribution*>(lo_solver);
            RD_ptr->SampleVelocity(FaceType::Interior);
            RD_ptr->SampleVelocity(FaceType::Boundary);
            RD_ptr->SetupPA(FaceType::Interior);
            RD_ptr->SetupPA(FaceType::Boundary);
         }
      }
      else
      {
         lo_solver = new ResidualDistribution(pfes, k, asmbl, lumpedM,
                                              subcell_scheme, time_dep);
      }
   }
   else if (lo_type == LOSolverType::ResDistSubcell)
   {
      const bool subcell_scheme = true;
      if (pa)
      {
         lo_solver = new PAResidualDistributionSubcell(pfes, k, asmbl, lumpedM,
                                                       subcell_scheme, time_dep);
         if (exec_mode == 0)
         {
            const PAResidualDistributionSubcell *RD_ptr =
               dynamic_cast<const PAResidualDistributionSubcell*>(lo_solver);
            RD_ptr->SampleVelocity(FaceType::Interior);
            RD_ptr->SampleVelocity(FaceType::Boundary);
            RD_ptr->SetupPA(FaceType::Interior);
            RD_ptr->SetupPA(FaceType::Boundary);
         }
      }
      else
      {
         lo_solver = new ResidualDistribution(pfes, k, asmbl, lumpedM,
                                              subcell_scheme, time_dep);
      }
   }
   else if (lo_type == LOSolverType::MassBased)
   {
      MFEM_VERIFY(ho_solver != nullptr,
                  "Mass-Based LO solver requires a choice of a HO solver.");
      lo_solver = new MassBasedAvg(pfes, *ho_solver,
                                   (exec_mode == 1) ? &v_gf : nullptr);
   }

   // Setup of the monolithic solver (if any).
   MonolithicSolver *mono_solver = NULL;
   bool mass_lim = (problem_num != 6 && problem_num != 7) ? true : false;
   if (mono_type == MonolithicSolverType::ResDistMono)
   {
      const bool subcell_scheme = false;
      mono_solver = new MonoRDSolver(pfes, k.SpMat(), m.SpMat(), lumpedM,
                                     asmbl, smth_indicator, velocity,
                                     subcell_scheme, time_dep, mass_lim);
   }
   else if (mono_type == MonolithicSolverType::ResDistMonoSubcell)
   {
      const bool subcell_scheme = true;
      mono_solver = new MonoRDSolver(pfes, k.SpMat(), m.SpMat(), lumpedM,
                                     asmbl, smth_indicator, velocity,
                                     subcell_scheme, time_dep, mass_lim);
   }

   // Print the starting meshes and initial condition.
   ofstream meshHO("meshHO_init.mesh");
   meshHO.precision(precision);
   pmesh.PrintAsOne(meshHO);
   if (subcell_mesh)
   {
      ofstream meshLO("meshLO_init.mesh");
      meshLO.precision(precision);
      subcell_mesh->PrintAsOne(meshLO);
   }
   ofstream sltn("sltn_init.gf");
   sltn.precision(precision);
   u.SaveAsOne(sltn);

   // Create data collection for solution output: either VisItDataCollection for
   // ASCII data files, or SidreDataCollection for binary data files.
   DataCollection *dc = NULL;
   if (visit)
   {
      dc = new VisItDataCollection("Remhos", &pmesh);
      dc->SetPrecision(precision);
      dc->RegisterField("solution", &u);
      dc->SetCycle(0);
      dc->SetTime(0.0);
      dc->Save();
   }

   socketstream sout, vis_s, vis_us;
   char vishost[] = "localhost";
   int  visport   = 19916;
   if (visualization)
   {
      // Make sure all MPI ranks have sent their 'v' solution before initiating
      // another set of GLVis connections (one from each rank):
      MPI_Barrier(pmesh.GetComm());

      sout.precision(8);
      vis_s.precision(8);
      vis_us.precision(8);

      u.HostRead();
      s.HostRead();
      VisualizeField(sout, vishost, visport,
                     u, "Solution u",
                     Wx, Wy, Ww, Wh, "gAmRj");
      if (product_sync)
      {
         VisualizeField(vis_s, vishost, visport, s, "Solution s",
                        Wx + Ww, Wy, Ww, Wh);
         VisualizeField(vis_us, vishost, visport, us, "Solution u_s",
                        Wx + 2*Ww, Wy, Ww, Wh);
      }
   }

   // Record the initial mass.
   Vector masses(lumpedM);
   const double mass0_u_loc = lumpedM * u;
   double mass0_u, mass0_us;
   MPI_Allreduce(&mass0_u_loc, &mass0_u, 1, MPI_DOUBLE, MPI_SUM, comm);
   if (product_sync)
   {
      const double mass0_us_loc = lumpedM * us;
      MPI_Allreduce(&mass0_us_loc, &mass0_us, 1, MPI_DOUBLE, MPI_SUM, comm);
   }

   /*{
      double mass_u, mass_u_loc = masses * u;
      MPI_Allreduce(&mass_u_loc, &mass_u, 1, MPI_DOUBLE, MPI_SUM, comm);
      if (myid == 0)
      {
         std::cout << setprecision(10)
                   << "Initial mass u: " << mass_u << std::endl
                   << "   Mass loss u: " << abs(mass0_u - mass_u) << std::endl;
      }
   }*/

   // Setup of the FCT solver (if any).
   Array<int> K_HO_smap;
   FCTSolver *fct_solver = NULL;
   if (fct_type == FCTSolverType::FluxBased)
   {
      MFEM_VERIFY(pa == false, "Flux-based FCT and PA are incompatible.");
      K_HO.SpMat().HostReadI();
      K_HO.SpMat().HostReadJ();
      K_HO.SpMat().HostReadData();
      K_HO_smap = SparseMatrix_Build_smap(K_HO.SpMat());
      const int fct_iterations = 1;
      fct_solver = new FluxBasedFCT(pfes, smth_indicator, dt, K_HO.SpMat(),
                                    K_HO_smap, M_HO.SpMat(), fct_iterations);
   }
   else if (fct_type == FCTSolverType::ClipScale)
   {
      fct_solver = new ClipScaleSolver(pfes, smth_indicator, dt);
   }
   else if (fct_type == FCTSolverType::NonlinearPenalty)
   {
      fct_solver = new NonlinearPenaltySolver(pfes, smth_indicator, dt);
   }
   else if (fct_type == FCTSolverType::FCTProject)
   {
      fct_solver = new ElementFCTProjection(pfes, dt);
   }

   AdvectionOperator adv(S.Size(), m, ml, lumpedM, k, M_HO, K_HO,
                         x, xsub, v_gf, v_sub_gf, asmbl, lom, dofs,
                         ho_solver, lo_solver, fct_solver, mono_solver);

   double t = 0.0;
   adv.SetTime(t);
   adv.SetTimeStepControl(dt_control);
   ode_solver->Init(adv);

   double umin, umax;
   GetMinMax(u, umin, umax);

   if (exec_mode == 1)
   {
      adv.SetRemapStartPos(x0, x0_sub);

      // For remap, the pseudo-time always evolves from 0 to 1.
      t_final = 1.0;
   }

   ParGridFunction res = u;
   double residual = 0.0;
   double s_min_glob = numeric_limits<double>::infinity(),
          s_max_glob = -numeric_limits<double>::infinity();

   // AMR operator
   amr::Operator *AMR = nullptr;
   if (amr) { AMR = new amr::Operator(pfes, pmesh, u, amr_options); }
   int depth = amr ? GetMeshDepth(pmesh) : 0;

   // Time-integration (loop over the time iterations, ti, with a time-step dt).
   bool done = false;
   BlockVector Sold(S);
   int ti_total = 0, ti = 0;
   while (done == false)
   {
      dbg("\033[31m###########################");
      dbg("\033[31m######## TIME LOOP ########");
      dbg("\033[31m######## dt:%f ########\033[m",dt);
      double dt_real = min(dt, t_final - t);

      // This also resets the time step estimate when automatic dt is on.
      adv.SetDt(dt_real);
      if (lo_solver)  { lo_solver->UpdateTimeStep(dt_real); }
      if (fct_solver) { fct_solver->UpdateTimeStep(dt_real); }

      // The inner refinement loop. At the end we want to have the current
      // time step resolved to the prescribed tolerance in each element.
      if (amr)
      {
         AMR->Reset();
         dbg("\t\033[33m##########################");
         dbg("\t\033[33m######## AMR LOOP ########");
         for (int ref_it = 1; ; ref_it++)
         {
            const int new_depth = mfem::GetMeshDepth(pmesh);
            if (new_depth > depth) { dt /= dt_factor; }
            if (new_depth < depth) { dt *= dt_factor; }
            if (new_depth != depth)
            {
               double dt_real = min(dt, t_final - t);
               adv.SetDt(dt_real);
               depth = new_depth;
               if (myid == 0)
               {
                  std::cout << "time step: " << ti
                            << ", time: " << t
                            << ", dt: " << dt;
                  std::cout << endl;
               }
            }
            //pmesh.pncmesh->PrintStats(std::cout);

            dbg("AMR->Apply");
            AMR->Apply();

            if (!AMR->Refined()) { dbg("Refines STOP!");  break; }

            dbg("AMR->Update");
            AMR->Update(adv, ode_solver, S, offset,
                        lom, subcell_mesh, pfes_sub, xsub, v_sub_gf,
                        mass0_u, inflow_gf, inflow);
            /*if (visualization)
            {
               MPI_Barrier(pmesh.GetComm());
               u.HostRead();
               VisualizeField(sout, vishost, visport, u, "Solution",
                              Wx, Wy, Ww, Wh);
            }*/
         }
         /*if (myid == 0)
         {
            ml.SpMat().GetDiag(lumpedM);
            masses = lumpedM;
            double mass_u, mass_u_loc = masses * u;
            MPI_Allreduce(&mass_u_loc, &mass_u, 1, MPI_DOUBLE, MPI_SUM, comm);
            std::cout << setprecision(10)
                      << "Before DEREFINMENT, u size:" << u.Size() << std::endl
                      << "Current mass u: " << mass_u << std::endl
                      << "   Mass loss u: " << abs(mass0_u - mass_u) << std::endl;
         }*/
         assert (!AMR->Refined());
         if (AMR->DeRefined())
         {
            dbg("DEREFINE, AMR->Update");
            AMR->Update(adv, ode_solver, S, offset,
                        lom, subcell_mesh, pfes_sub, xsub, v_sub_gf,
                        mass0_u, inflow_gf, inflow);
            /*if (visualization)
            {
               MPI_Barrier(pmesh.GetComm());
               u.HostRead();
               VisualizeField(sout, vishost, visport, u, "Solution",
                              Wx, Wy, Ww, Wh);
            }*/
            //assert(false);
         }
      }

      if (product_sync)
      {
         ComputeMinMaxS(pmesh.GetNE(), us, u, s_min_glob, s_max_glob);
#ifdef REMHOS_FCT_PRODUCT_DEBUG
         if (myid == 0)
         {
            std::cout << "   --- Full time step" << std::endl;
            std::cout << "   in:  ";
            std::cout << std::scientific << std::setprecision(5);
            std::cout << "min_s: " << s_min_glob
                      << "; max_s: " << s_max_glob << std::endl;
         }
#endif
      }

      Sold = S;
      ode_solver->Step(S, t, dt_real);
      ti++;
      ti_total++;

      if (dt_control != TimeStepControl::FixedTimeStep)
      {
         double dt_est = adv.GetTimeStepEstimate();
         if (dt_est < dt_real)
         {
            // Repeat with the proper time step.
            if (myid == 0)
            {
               cout << "Repeat / decrease dt: "
                    << dt_real << " --> " << 0.85 * dt << endl;
            }
            ti--;
            t -= dt_real;
            S  = Sold;
            dt = 0.85 * dt;
            if (dt < 1e-12) { MFEM_ABORT("The time step crashed!"); }
            continue;
         }
         else if (dt_est > 1.25 * dt_real) { dt *= 1.02; }
      }

      // S has been modified, update the alias
      u.SyncMemory(S);
      if (product_sync)
      {
         us.SyncMemory(S);

         // It is known that RK time integrators with more than 1 stage may
         // cause violation of the lower bounds for us.
         // The lower bound is corrected, causing small conservation error.
         // Correction can also be done with localized bounds for s, but for
         // now we have implemented only the minimum global bound.
         u.HostRead();
         us.HostReadWrite();
         const int s = u.Size();
         Array<bool> active_elem, active_dofs;
         ComputeBoolIndicators(NE, u, active_elem, active_dofs);
         for (int i = 0; i < s; i++)
         {
            if (active_dofs[i] == false) { continue; }

            double us_min = u(i) * s_min_glob;
            if (us(i) < us_min) { us(i) = us_min; }
         }

#ifdef REMHOS_FCT_PRODUCT_DEBUG
         ComputeMinMaxS(NE, us, u, s_min_glob, s_max_glob);
         if (myid == 0)
         {
            std::cout << "   out: ";
            std::cout << std::scientific << std::setprecision(5);
            std::cout << "min_s: " << s_min_glob
                      << "; max_s: " << s_max_glob << std::endl;
         }
#endif
      }

      /*{
         Vector tmp;
         u.GetTrueDofs(tmp);
         u.SetFromTrueDofs(tmp);
      }*/

      // Monotonicity check for debug purposes mainly.
      if (verify_bounds && forced_bounds && smth_indicator == NULL)
      {
         double umin_new, umax_new;
         GetMinMax(u, umin_new, umax_new);
         if (problem_num % 10 != 6 && problem_num % 10 != 7)
         {
            if (myid == 0)
            {
               MFEM_VERIFY(umin_new > umin - 1e-12,
                           "Undershoot of " << umin - umin_new);
               MFEM_VERIFY(umax_new < umax + 1e-12,
                           "Overshoot of " << umax_new - umax);
            }
            umin = umin_new;
            umax = umax_new;
         }
         else
         {
            if (myid == 0)
            {
               MFEM_VERIFY(umin_new > 0.0 - 1e-12,
                           "Undershoot of " << 0.0 - umin_new);
               MFEM_VERIFY(umax_new < 1.0 + 1e-12,
                           "Overshoot of " << umax_new - 1.0);
            }
         }
      }

      if (exec_mode == 1)
      {
         x0.HostReadWrite(); v_sub_gf.HostReadWrite();
         x.HostReadWrite();
         add(x0, t, v_gf, x);
         x0_sub.HostReadWrite(); v_sub_gf.HostReadWrite();
         MFEM_VERIFY(xsub != NULL,
                     "xsub == NULL/This code should not be entered for order = 1.");
         xsub->HostReadWrite();
         add(x0_sub, t, v_sub_gf, *xsub);
      }

      if (problem_num != 6 && problem_num != 7 && problem_num != 8)
      {
         done = (t >= t_final - 1.e-8*dt);
      }
      else
      {
         // Steady state problems - stop at convergence.
         double res_loc = 0.;
         lumpedM.HostReadWrite(); u.HostReadWrite(); res.HostReadWrite();
         for (int i = 0; i < res.Size(); i++)
         {
            res_loc += pow( (lumpedM(i) * u(i) / dt) - (lumpedM(i) * res(i) / dt), 2. );
         }
         MPI_Allreduce(&res_loc, &residual, 1, MPI_DOUBLE, MPI_SUM, comm);

         residual = sqrt(residual);
         if (residual < 1.e-12 && t >= 1.) { done = true; u = res; }
         else { res = u; }
      }

      if (ti == max_tsteps) { dbg("max_tsteps reached!"); done = true; }

      if (done || ti % vis_steps == 0)
      {
         if (myid == 0)
         {
            /*
            cout << "time step: " << ti << ", time: " << t << ", dt: " << dt;
            if (steady_state_problem) { cout << ", residual: " << residual ; }
            cout << endl;
            */
            cout << "time step: " << ti << ", time: " << t
                 << ", dt: " << dt << ", residual: " << residual << endl;
         }

         if (visualization)
         {
            MPI_Barrier(pmesh.GetComm());
            u.HostRead();
            VisualizeField(sout, vishost, visport, u, "Solution",
                           Wx, Wy, Ww, Wh);
            if (product_sync)
            {
               // Recompute s = u_s / u.
               ComputeRatio(pmesh.GetNE(), us, u, s, u_bool_el, u_bool_dofs);
               VisualizeField(vis_s, vishost, visport, s, "Solution s",
                              Wx + Ww, Wy, Ww, Wh);
               VisualizeField(vis_us, vishost, visport, us, "Solution u_s",
                              Wx + 2*Ww, Wy, Ww, Wh);
            }
         }

         if (visit)
         {
            dc->SetCycle(ti);
            dc->SetTime(t);
            dc->Save();
         }
      }
   }

   if (dt_control != TimeStepControl::FixedTimeStep && myid == 0)
   {
      cout << "Total time steps: " << ti_total
           << " (" << ti_total-ti << " repeated)." << endl;
   }

   // Print the final meshes and solution.
   {
      ofstream meshHO("meshHO_final.mesh");
      meshHO.precision(precision);
      pmesh.PrintAsOne(meshHO);
      if (subcell_mesh)
      {
         ofstream meshLO("meshLO_final.mesh");
         meshLO.precision(precision);
         subcell_mesh->PrintAsOne(meshLO);
      }
      ofstream sltn("sltn_final.gf");
      sltn.precision(precision);
      u.SaveAsOne(sltn);
   }

   // Check for mass conservation.
   double mass_u_loc = 0.0, mass_us_loc = 0.0;
   if (exec_mode == 1)
   {
      ml.BilinearForm::operator=(0.0);
      ml.Assemble();
      lumpedM.HostRead();
      ml.SpMat().GetDiag(lumpedM);
      mass_u_loc = lumpedM * u;
      if (product_sync) { mass_us_loc = lumpedM * us; }
   }
   else
   {
      /*ml.SpMat().GetDiag(lumpedM);
      masses = lumpedM;*/
      mass_u_loc = masses * u;
      if (product_sync) { mass_us_loc = masses * us; }
   }
   double mass_u, mass_us, s_max;
   MPI_Allreduce(&mass_u_loc, &mass_u, 1, MPI_DOUBLE, MPI_SUM, comm);
   const double umax_loc = u.Max();
   MPI_Allreduce(&umax_loc, &umax, 1, MPI_DOUBLE, MPI_MAX, comm);
   if (product_sync)
   {
      ComputeRatio(pmesh.GetNE(), us, u, s, u_bool_el, u_bool_dofs);
      const double s_max_loc = s.Max();
      MPI_Allreduce(&mass_us_loc, &mass_us, 1, MPI_DOUBLE, MPI_SUM, comm);
      MPI_Allreduce(&s_max_loc, &s_max, 1, MPI_DOUBLE, MPI_MAX, comm);
   }
   if (myid == 0)
   {
      cout << setprecision(10)
           << "Final mass u:  " << mass_u << endl
           << "Max value u:   " << umax << endl << setprecision(6)
           << "Mass loss u:   " << abs(mass0_u - mass_u) << endl;
      if (product_sync)
      {
         cout << setprecision(10)
              << "Final mass us: " << mass_us << endl
              << "Max value s:   " << s_max << endl << setprecision(6)
              << "Mass loss us:  " << abs(mass0_us - mass_us) << endl;
      }
   }

   // Compute errors, if the initial condition is equal to the final solution
   if (problem_num == 4) // solid body rotation
   {
      double err = u.ComputeLpError(1., u0);
      if (myid == 0) { cout << "L1-error: " << err << "." << endl; }
   }
   else if (problem_num == 7)
   {
      FunctionCoefficient u_ex(inflow_function);
      double e1 = u.ComputeLpError(1., u_ex);
      double e2 = u.ComputeLpError(2., u_ex);
      double eInf = u.ComputeLpError(numeric_limits<double>::infinity(), u_ex);
      if (myid == 0)
      {
         cout << "L1-error: " << e1 << "." << endl;

         // write output
         ofstream file("errors.txt", ios_base::app);

         if (!file)
         {
            MFEM_ABORT("Error opening file.");
         }
         else
         {
            ostringstream strs;
            strs << e1 << " " << e2 << " " << eInf << "\n";
            string str = strs.str();
            file << str;
            file.close();
         }
      }
   }

   if (smth_indicator)
   {
      // Print the values of the smoothness indicator.
      ParGridFunction si_val;
      smth_indicator->ComputeSmoothnessIndicator(u, si_val);
      {
         ofstream smth("si_final.gf");
         smth.precision(precision);
         si_val.SaveAsOne(smth);
      }
   }

   // Free the used memory.
   delete mono_solver;
   delete fct_solver;
   delete smth_indicator;
   delete ho_solver;

   delete ode_solver;
   delete mesh_fec;
   delete lom.pk;
   delete dc;

   if (order > 1)
   {
      delete subcell_mesh;
      delete fec_sub;
      delete pfes_sub;
      delete xsub;
      delete lom.SubFes0;
      delete lom.SubFes1;
      delete lom.VolumeTerms;
   }

   return 0;
}
