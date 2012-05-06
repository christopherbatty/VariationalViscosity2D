#include "fluidsim.h"

#include "array2_utils.h"

#include "pcgsolver/sparse_matrix.h"
#include "pcgsolver/pcg_solver.h"

float fraction_inside(float phi_left, float phi_right);
void extrapolate(Array2f& grid, Array2c& valid);

float circle_phi(const Vec2f& pos) {
   Vec2f centre(0.5f,0.75f);
   float rad = 0.1f;
   Vec2f centre1(0.4f, 0.3f);
   float rad1 = 0.15f;
   float phi0 = dist(centre, pos) - rad;
   float phi1 = dist(centre1, pos) - rad1;
   return min(phi0,phi1);
}

void FluidSim::initialize(float width, int ni_, int nj_) {
   ni = ni_;
   nj = nj_;
   dx = width / (float)ni;
   u.resize(ni+1,nj); temp_u.resize(ni+1,nj); u_weights.resize(ni+1,nj); u_valid.resize(ni+1,nj); u_vol.resize(ni+1,nj);
   v.resize(ni,nj+1); temp_v.resize(ni,nj+1); v_weights.resize(ni,nj+1); v_valid.resize(ni,nj+1); v_vol.resize(ni,nj+1);
   c_vol.resize(ni,nj);
   n_vol.resize(ni+1,nj+1);
   u.set_zero();
   v.set_zero();
   nodal_solid_phi.resize(ni+1,nj+1);
   valid.resize(ni+1, nj+1);
   old_valid.resize(ni+1, nj+1);
   liquid_phi.resize(ni,nj);
   particle_radius = dx/sqrt(2.0f);
   viscosity.resize(ni,nj);
   viscosity.assign(1.0f);
   //surface.reset_phi(circle_phi, dx, Vec2f(0.5*dx,0.5*dx), ni, nj);
}

//Initialize the grid-based signed distance field that dictates the position of the solid boundary
void FluidSim::set_boundary(float (*phi)(const Vec2f&)) {
   
   for(int j = 0; j < nj+1; ++j) for(int i = 0; i < ni+1; ++i) {
      Vec2f pos(i*dx,j*dx);
      nodal_solid_phi(i,j) = phi(pos);
   }

}

float FluidSim::cfl() {
   float maxvel = 0;
   for(unsigned int i = 0; i < u.a.size(); ++i)
      maxvel = max(maxvel, fabs(u.a[i]));
   for(unsigned int i = 0; i < v.a.size(); ++i)
      maxvel = max(maxvel, fabs(v.a[i]));
   return dx / maxvel;
}

//The main fluid simulation step
void FluidSim::advance(float dt) {
   float t = 0;
   
   while(t < dt) {
      float substep = cfl();   
      if(t + substep > dt)
         substep = dt - t;
   
      //Passively advect particles
      advect_particles(substep);
     
      //Estimate the liquid signed distance
      compute_phi();

      //Advance the velocity
      advect(substep);
      add_force(substep);

      apply_viscosity(substep);

      apply_projection(substep); 
      
      //Pressure projection only produces valid velocities in faces with non-zero associated face area.
      //Because the advection step may interpolate from these invalid faces, 
      //we must extrapolate velocities from the fluid domain into these zero-area faces.
      extrapolate(u, u_valid);
      extrapolate(v, v_valid);

      //For extrapolated velocities, replace the normal component with
      //that of the object.
      constrain_velocity();
   
      t+=substep;
   }
}

void FluidSim::add_force(float dt) {
   
   for(int j = 0; j < nj+1; ++j) for(int i = 0; i < ni; ++i) {
      v(i,j) -= 0.1f;
   }
   
}

//For extrapolated points, replace the normal component
//of velocity with the object velocity (in this case zero).
void FluidSim::constrain_velocity() {
   temp_u = u;
   temp_v = v;
   
   //(At lower grid resolutions, the normal estimate from the signed
   //distance function is poor, so it doesn't work quite as well.
   //An exact normal would do better.)
   
   //constrain u
   for(int j = 0; j < u.nj; ++j) for(int i = 0; i < u.ni; ++i) {
      if(u_weights(i,j) == 0) {
         //apply constraint
         Vec2f pos(i*dx, (j+0.5f)*dx);
         Vec2f vel = get_velocity(pos);
         Vec2f normal(0,0);
         interpolate_gradient(normal, pos/dx, nodal_solid_phi); 
         normalize(normal);
         float perp_component = dot(vel, normal);
         vel -= perp_component*normal;
         temp_u(i,j) = vel[0];
      }
   }
   
   //constrain v
   for(int j = 0; j < v.nj; ++j) for(int i = 0; i < v.ni; ++i) {
      if(v_weights(i,j) == 0) {
         //apply constraint
         Vec2f pos((i+0.5f)*dx, j*dx);
         Vec2f vel = get_velocity(pos);
         Vec2f normal(0,0);
         interpolate_gradient(normal, pos/dx, nodal_solid_phi); 
         normalize(normal);
         float perp_component = dot(vel, normal);
         vel -= perp_component*normal;
         temp_v(i,j) = vel[1];
      }
   }
   
   //update
   u = temp_u;
   v = temp_v;

}

//Add a tracer particle for visualization
void FluidSim::add_particle(const Vec2f& position) {
   particles.push_back(position);
}

//Basic first order semi-Lagrangian advection of velocities
void FluidSim::advect(float dt) {
   
   //semi-Lagrangian advection on u-component of velocity
   for(int j = 0; j < nj; ++j) for(int i = 0; i < ni+1; ++i) {
      Vec2f pos(i*dx, (j+0.5f)*dx);
      pos = trace_rk2(pos, -dt);
      temp_u(i,j) = get_velocity(pos)[0];  
   }

   //semi-Lagrangian advection on v-component of velocity
   for(int j = 0; j < nj+1; ++j) for(int i = 0; i < ni; ++i) {
      Vec2f pos((i+0.5f)*dx, j*dx);
      pos = trace_rk2(pos, -dt);
      temp_v(i,j) = get_velocity(pos)[1];
   }

   //move update velocities into u/v vectors
   u = temp_u;
   v = temp_v;
}

//Perform 2nd order Runge Kutta to move the particles in the fluid
void FluidSim::advect_particles(float dt) {
   
   for(unsigned int p = 0; p < particles.size(); ++p) {
      Vec2f before = particles[p];
      Vec2f start_velocity = get_velocity(before);
      Vec2f midpoint = before + 0.5f*dt*start_velocity;
      Vec2f mid_velocity = get_velocity(midpoint);
      particles[p] += dt*mid_velocity;
      Vec2f after = particles[p];
      if(dist(before,after) > 3*dx) {
         std::cout << "Before: " << before << " " << "After: " << after << std::endl;
         std::cout << "Mid point: " << midpoint << std::endl;
         std::cout << "Start velocity: " << start_velocity << "  Time step: " << dt << std::endl;
         std::cout << "Mid velocity: " << mid_velocity << std::endl;
      }

      //Particles can still occasionally leave the domain due to truncation errors, 
      //interpolation error, or large timesteps, so we project them back in for good measure.
      
      //Try commenting this section out to see the degree of accumulated error.
      float phi_value = interpolate_value(particles[p]/dx, nodal_solid_phi);
      if(phi_value < 0) {
         Vec2f normal;
         interpolate_gradient(normal, particles[p]/dx, nodal_solid_phi);        
         normalize(normal);
         particles[p] -= phi_value*normal;
      }
   }

}


void FluidSim::compute_phi() {
   
   //Estimate from particles
   liquid_phi.assign(3*dx);
   for(unsigned int p = 0; p < particles.size(); ++p) {
      Vec2f point = particles[p];
      int i,j;
      float fx,fy;
      //determine containing cell;
      get_barycentric((point[0])/dx-0.5f, i, fx, 0, ni);
      get_barycentric((point[1])/dx-0.5f, j, fy, 0, nj);
      
      //compute distance to surrounding few points, keep if it's the minimum
      for(int j_off = j-2; j_off<=j+2; ++j_off) for(int i_off = i-2; i_off<=i+2; ++i_off) {
         if(i_off < 0 || i_off >= ni || j_off < 0 || j_off >= nj)
            continue;
         
         Vec2f pos((i_off+0.5f)*dx, (j_off+0.5f)*dx);
         float phi_temp = dist(pos, point) - 1.02f*particle_radius;
         liquid_phi(i_off,j_off) = min(liquid_phi(i_off,j_off), phi_temp);
      }
   }
   
   //"extrapolate" phi into solids if nearby
   for(int j = 0; j < nj; ++j) {
      for(int i = 0; i < ni; ++i) {
         if(liquid_phi(i,j) < 0.5*dx) {
            float solid_phi_val = 0.25f*(nodal_solid_phi(i,j) + nodal_solid_phi(i+1,j) + nodal_solid_phi(i,j+1) + nodal_solid_phi(i+1,j+1));
            if(solid_phi_val < 0)
               liquid_phi(i,j) = -0.5f*dx;
         }
      }
   }  
}



void FluidSim::apply_projection(float dt) {

   //Compute finite-volume type face area weight for each velocity sample.
   compute_pressure_weights();
   
   //Set up and solve the variational pressure solve.
   solve_pressure(dt);

}

void FluidSim::apply_viscosity(float dt) {
   
   printf("Computing weights\n");
   //Estimate weights at velocity and stress positions
   compute_viscosity_weights();

   printf("Setting up solve\n");
   //Set up and solve the linear system
   solve_viscosity(dt);

}  

//Apply RK2 to advect a point in the domain.
Vec2f FluidSim::trace_rk2(const Vec2f& position, float dt) {
   Vec2f input = position;
   Vec2f velocity = get_velocity(input);
   velocity = get_velocity(input + 0.5f*dt*velocity);
   input += dt*velocity;
   return input;
}

//Interpolate velocity from the MAC grid.
Vec2f FluidSim::get_velocity(const Vec2f& position) {
   
   //Interpolate the velocity from the u and v grids
   float u_value = interpolate_value(position / dx - Vec2f(0, 0.5f), u);
   float v_value = interpolate_value(position / dx - Vec2f(0.5f, 0), v);
   
   return Vec2f(u_value, v_value);
}


//Given two signed distance values, determine what fraction of a connecting segment is "inside"
float fraction_inside(float phi_left, float phi_right) {
   if(phi_left < 0 && phi_right < 0)
      return 1;
   if (phi_left < 0 && phi_right >= 0)
      return phi_left / (phi_left - phi_right);
   if(phi_left >= 0 && phi_right < 0)
      return phi_right / (phi_right - phi_left);
   else
      return 0;
}

//Compute finite-volume style face-weights for fluid from nodal signed distances
void FluidSim::compute_pressure_weights() {

   for(int j = 0; j < u_weights.nj; ++j) for(int i = 0; i < u_weights.ni; ++i) {
      u_weights(i,j) = 1 - fraction_inside(nodal_solid_phi(i,j+1), nodal_solid_phi(i,j));
      u_weights(i,j) = clamp(u_weights(i,j), 0.0f, 1.0f);
   }
   for(int j = 0; j < v_weights.nj; ++j) for(int i = 0; i < v_weights.ni; ++i) {
      v_weights(i,j) = 1 - fraction_inside(nodal_solid_phi(i+1,j), nodal_solid_phi(i,j));
      v_weights(i,j) = clamp(v_weights(i,j), 0.0f, 1.0f);
   }

}


void compute_volume_fractions(const Array2f& levelset, Array2f& fractions, Vec2f fraction_origin, int subdivision) {
   
   //Assumes levelset and fractions have the same dx
   float sub_dx = 1.0f / subdivision;
   int sample_max = subdivision*subdivision;
   for(int j = 0; j < fractions.nj; ++j) {
      for(int i = 0; i < fractions.ni; ++i) {
         float start_x = fraction_origin[0] + (float)i;
         float start_y = fraction_origin[1] + (float)j;
         int incount = 0;

         for(int sub_j = 0; sub_j < subdivision; ++sub_j) {
            for(int sub_i = 0; sub_i < subdivision; ++sub_i) {
               float x_pos = start_x + (sub_i+0.5f)*sub_dx;
               float y_pos = start_y + (sub_j+0.5f)*sub_dx;
               float phi_val = interpolate_value(Vec2f(x_pos,y_pos), levelset);
               if(phi_val < 0) 
                  ++incount;
            }
         }
         fractions(i,j) = (float)incount / (float)sample_max;
      }
   }

}

void FluidSim::compute_viscosity_weights() {
   
   compute_volume_fractions(liquid_phi, c_vol, Vec2f(-0.5,-0.5), 2);
   compute_volume_fractions(liquid_phi, n_vol, Vec2f(-1, -1), 2);
   compute_volume_fractions(liquid_phi, u_vol, Vec2f(-1,-0.5), 2);
   compute_volume_fractions(liquid_phi, v_vol, Vec2f(-0.5,-1), 2);

}

//An implementation of the variational pressure projection solve for static geometry
void FluidSim::solve_pressure(float dt) {
   
   //This linear system could be simplified, but I've left it as is for clarity 
   //and consistency with the standard naive discretization
   
   int ni = v.ni;
   int nj = u.nj;
   int system_size = ni*nj;
   if(rhs.size() != system_size) {
      rhs.resize(system_size);
      pressure.resize(system_size);
      matrix.resize(system_size);
   }
   matrix.zero();
   
   //Build the linear system for pressure
   for(int j = 1; j < nj-1; ++j) {
      for(int i = 1; i < ni-1; ++i) {
         int index = i + ni*j;
         rhs[index] = 0;
         pressure[index] = 0;
         float centre_phi = liquid_phi(i,j);
         if(centre_phi < 0) {

            //right neighbour
            float term = u_weights(i+1,j) * dt / sqr(dx);
            float right_phi = liquid_phi(i+1,j);
            if(right_phi < 0) {
               matrix.add_to_element(index, index, term);
               matrix.add_to_element(index, index + 1, -term);
            }
            else {
               float theta = fraction_inside(centre_phi, right_phi);
               if(theta < 0.01f) theta = 0.01f;
               matrix.add_to_element(index, index, term/theta);
            }
            rhs[index] -= u_weights(i+1,j)*u(i+1,j) / dx;
            
            //left neighbour
            term = u_weights(i,j) * dt / sqr(dx);
            float left_phi = liquid_phi(i-1,j);
            if(left_phi < 0) {
               matrix.add_to_element(index, index, term);
               matrix.add_to_element(index, index - 1, -term);
            }
            else {
               float theta = fraction_inside(centre_phi, left_phi);
               if(theta < 0.01f) theta = 0.01f;
               matrix.add_to_element(index, index, term/theta);
            }
            rhs[index] += u_weights(i,j)*u(i,j) / dx;
            
            //top neighbour
            term = v_weights(i,j+1) * dt / sqr(dx);
            float top_phi = liquid_phi(i,j+1);
            if(top_phi < 0) {
               matrix.add_to_element(index, index, term);
               matrix.add_to_element(index, index + ni, -term);
            }
            else {
               float theta = fraction_inside(centre_phi, top_phi);
               if(theta < 0.01f) theta = 0.01f;
               matrix.add_to_element(index, index, term/theta);
            }
            rhs[index] -= v_weights(i,j+1)*v(i,j+1) / dx;
            
            //bottom neighbour
            term = v_weights(i,j) * dt / sqr(dx);
            float bot_phi = liquid_phi(i,j-1);
            if(bot_phi < 0) {
               matrix.add_to_element(index, index, term);
               matrix.add_to_element(index, index - ni, -term);
            }
            else {
               float theta = fraction_inside(centre_phi, bot_phi);
               if(theta < 0.01f) theta = 0.01f;
               matrix.add_to_element(index, index, term/theta);
            }
            rhs[index] += v_weights(i,j)*v(i,j) / dx;
         }
      }
   }

   //Solve the system using Robert Bridson's incomplete Cholesky PCG solver
   
   double tolerance;
   int iterations;
   bool success = solver.solve(matrix, rhs, pressure, tolerance, iterations);
   if(!success) {
      printf("WARNING: Pressure solve failed!************************************************\n");
   }
   
   //Apply the velocity update
   u_valid.assign(0);
   for(int j = 0; j < u.nj; ++j) for(int i = 1; i < u.ni-1; ++i) {
      int index = i + j*ni;
      if(u_weights(i,j) > 0 && (liquid_phi(i,j) < 0 || liquid_phi(i-1,j) < 0)) {
         float theta = 1;
         if(liquid_phi(i,j) >= 0 || liquid_phi(i-1,j) >= 0)
            theta = fraction_inside(liquid_phi(i-1,j), liquid_phi(i,j));
         if(theta < 0.01f) theta = 0.01f;
         u(i,j) -= dt  * (float)(pressure[index] - pressure[index-1]) / dx / theta; 
         u_valid(i,j) = 1;
      }
      else
         u(i,j) = 0;
   }
   v_valid.assign(0);
   for(int j = 1; j < v.nj-1; ++j) for(int i = 0; i < v.ni; ++i) {
      int index = i + j*ni;
      if(v_weights(i,j) > 0 && (liquid_phi(i,j) < 0 || liquid_phi(i,j-1) < 0)) {
         float theta = 1;
         if(liquid_phi(i,j) >= 0 || liquid_phi(i,j-1) >= 0)
            theta = fraction_inside(liquid_phi(i,j-1), liquid_phi(i,j));
         if(theta < 0.01f) theta = 0.01f;
         v(i,j) -= dt  * (float)(pressure[index] - pressure[index-ni]) / dx / theta; 
         v_valid(i,j) = 1;
      }
      else
         v(i,j) = 0;
   }

}

int FluidSim::u_ind(int i, int j) {
   return i + j*(ni+1);
}

int FluidSim::v_ind(int i, int j) {
   return i + j*ni + (ni+1)*nj;
}


void FluidSim::solve_viscosity(float dt) {
   int ni = liquid_phi.ni;
   int nj = liquid_phi.nj;
   
   //static obstacles for simplicity - for moving objects, 
   //use a spatially varying 2d array, and modify the linear system appropriately
   float u_obj = 0;
   float v_obj = 0;

   Array2c u_state(ni+1,nj,(const char&)0);
   Array2c v_state(ni,nj+1,(const char&)0);
   const int SOLID = 1;
   const int FLUID = 0;

   printf("Determining states\n");
   //just determine if the face position is inside the wall! That's it.
   for(int j = 0; j < nj; ++j) {
      for(int i = 0; i < ni+1; ++i) {
         if(i - 1 < 0 || i >= ni || (nodal_solid_phi(i,j+1) + nodal_solid_phi(i,j))/2 <= 0)
            u_state(i,j) = SOLID;
         else
            u_state(i,j) = FLUID;
      }
   }


   for(int j = 0; j < nj+1; ++j)  {
      for(int i = 0; i < ni; ++i) {
         if(j - 1 < 0 || j >= nj || (nodal_solid_phi(i+1,j) + nodal_solid_phi(i,j))/2 <= 0)
            v_state(i,j) = SOLID;
         else 
            v_state(i,j) = FLUID;
      }
   }
   
   printf("Building matrix\n");
   int elts = (ni+1)*nj + ni*(nj+1);
   if(vrhs.size() != elts) {
      vrhs.resize(elts);
      velocities.resize(elts);
      vmatrix.resize(elts);
   }
   vmatrix.zero();
   
   float factor = dt/sqr(dx);
   for(int j = 1; j < nj-1; ++j) for(int i = 1; i < ni-1; ++i) {
      if(u_state(i,j) == FLUID ) {
         int index = u_ind(i,j);      
         
         vrhs[index] = u_vol(i,j) * u(i,j);
         vmatrix.set_element(index,index,u_vol(i,j));
         
         //uxx terms
         float visc_right = viscosity(i,j);
         float visc_left = viscosity(i-1,j);
         float vol_right = c_vol(i,j);
         float vol_left = c_vol(i-1,j);

        //u_x_right
         vmatrix.add_to_element(index,index, 2*factor*visc_right*vol_right);
         if(u_state(i+1,j) == FLUID)
            vmatrix.add_to_element(index,u_ind(i+1,j), -2*factor*visc_right*vol_right);
         else if(u_state(i+1,j) == SOLID)
            vrhs[index] -= -2*factor*visc_right*vol_right*u_obj;

         //u_x_left
         vmatrix.add_to_element(index,index, 2*factor*visc_left*vol_left);
         if(u_state(i-1,j) == FLUID)
            vmatrix.add_to_element(index,u_ind(i-1,j), -2*factor*visc_left*vol_left);
         else if(u_state(i-1,j) == SOLID)
            vrhs[index] -= -2*factor*visc_left*vol_left*u_obj;
         
         //uyy terms
         float visc_top = 0.25f*(viscosity(i-1,j+1) + viscosity(i-1,j) + viscosity(i,j+1) + viscosity(i,j));
         float visc_bottom = 0.25f*(viscosity(i-1,j) + viscosity(i-1,j-1) + viscosity(i,j) + viscosity(i,j-1));
         float vol_top = n_vol(i,j+1);
         float vol_bottom = n_vol(i,j);

         //u_y_top
         vmatrix.add_to_element(index,index, +factor*visc_top*vol_top);
         if(u_state(i,j+1) == FLUID)
            vmatrix.add_to_element(index,u_ind(i,j+1), -factor*visc_top*vol_top);
         else if(u_state(i,j+1) == SOLID)
            vrhs[index] -= -u_obj*factor*visc_top*vol_top;
      
         //u_y_bottom
         vmatrix.add_to_element(index,index, +factor*visc_bottom*vol_bottom);
         if(u_state(i,j-1) == FLUID)
            vmatrix.add_to_element(index,u_ind(i,j-1), -factor*visc_bottom*vol_bottom);
         else if(u_state(i,j-1) == SOLID)
            vrhs[index] -= -u_obj*factor*visc_bottom*vol_bottom;
      
         //vxy terms
         //v_x_top
         if(v_state(i,j+1) == FLUID)
            vmatrix.add_to_element(index,v_ind(i,j+1), -factor*visc_top*vol_top);
         else if(v_state(i,j+1) == SOLID)
            vrhs[index] -= -v_obj*factor*visc_top*vol_top;
         
         if(v_state(i-1,j+1) == FLUID)
            vmatrix.add_to_element(index,v_ind(i-1,j+1), factor*visc_top*vol_top);
         else if(v_state(i-1,j+1) == SOLID)
            vrhs[index] -= v_obj*factor*visc_top*vol_top;
     
         //v_x_bottom
         if(v_state(i,j) == FLUID)
            vmatrix.add_to_element(index,v_ind(i,j), +factor*visc_bottom*vol_bottom);
         else if(v_state(i,j) == SOLID)
            vrhs[index] -= v_obj*factor*visc_bottom*vol_bottom;
         
         if(v_state(i-1,j) == FLUID)
            vmatrix.add_to_element(index,v_ind(i-1,j), -factor*visc_bottom*vol_bottom);
         else if(v_state(i-1,j) == SOLID)
            vrhs[index] -= -v_obj*factor*visc_bottom*vol_bottom;
      

      }
   }
   
   for(int j = 1; j < nj; ++j) for(int i = 1; i < ni-1; ++i) {
      if(v_state(i,j) == FLUID) {
         int index = v_ind(i,j);      
         
         vrhs[index] = v_vol(i,j)*v(i,j);
         vmatrix.set_element(index, index, v_vol(i,j));

         //vyy
         float visc_top = viscosity(i,j);
         float visc_bottom = viscosity(i,j-1);
         float vol_top = c_vol(i,j);
         float vol_bottom = c_vol(i,j-1);

         //vy_top
         vmatrix.add_to_element(index,index, +2*factor*visc_top*vol_top);
         if(v_state(i,j+1) == FLUID)
            vmatrix.add_to_element(index,v_ind(i,j+1), -2*factor*visc_top*vol_top);
         else if (v_state(i,j+1) == SOLID)
            vrhs[index] -= -2*factor*visc_top*vol_top*v_obj;
         
         //vy_bottom
         vmatrix.add_to_element(index,index, +2*factor*visc_bottom*vol_bottom);
         if(v_state(i,j-1) == FLUID)
            vmatrix.add_to_element(index,v_ind(i,j-1), -2*factor*visc_bottom*vol_bottom);
         else if(v_state(i,j-1) == SOLID)
            vrhs[index] -= -2*factor*visc_bottom*vol_bottom*v_obj;
         
         //vxx terms
         float visc_right = 0.25f*(viscosity(i,j-1) + viscosity(i+1,j-1) + viscosity(i,j) + viscosity(i+1,j));
         float visc_left = 0.25f*(viscosity(i,j-1) + viscosity(i-1,j-1) + viscosity(i,j) + viscosity(i-1,j));
         float vol_right = n_vol(i+1,j);
         float vol_left = n_vol(i,j);

         //v_x_right
         vmatrix.add_to_element(index,index, +factor*visc_right*vol_right);
         if(v_state(i+1,j) == FLUID)
            vmatrix.add_to_element(index,v_ind(i+1,j), -factor*visc_right*vol_right);
         else if(v_state(i+1,j) == SOLID)
            vrhs[index] -= -v_obj*factor*visc_right*vol_right;
      
         //v_x_left
         vmatrix.add_to_element(index,index, +factor*visc_left*vol_left);
         if(v_state(i-1,j) == FLUID)
            vmatrix.add_to_element(index,v_ind(i-1,j), -factor*visc_left*vol_left);
         else if(v_state(i-1,j) == SOLID)
            vrhs[index] -= -v_obj*factor*visc_left*vol_left;

         //uyx

         //u_y_right
         if(u_state(i+1,j) == FLUID)
            vmatrix.add_to_element(index,u_ind(i+1,j), -factor*visc_right*vol_right);
         else if(u_state(i+1,j) == SOLID)
            vrhs[index] -= -u_obj*factor*visc_right*vol_right;
         
         if(u_state(i+1,j-1) == FLUID)
            vmatrix.add_to_element(index,u_ind(i+1,j-1), factor*visc_right*vol_right);
         else if(u_state(i+1,j-1) == SOLID)
            vrhs[index] -= u_obj*factor*visc_right*vol_right;
      
         //u_y_left
         if(u_state(i,j) == FLUID)
            vmatrix.add_to_element(index,u_ind(i,j), factor*visc_left*vol_left);
         else if(u_state(i,j) == SOLID)
            vrhs[index] -= u_obj*factor*visc_left*vol_left;
          
         if(u_state(i,j-1) == FLUID)
            vmatrix.add_to_element(index,u_ind(i,j-1), -factor*visc_left*vol_left);
         else if(u_state(i,j-1) == SOLID)
            vrhs[index] -= -u_obj*factor*visc_left*vol_left;
      
      }
   }

   
   double res_out;
   int iter_out;
   
   solver.solve(vmatrix, vrhs, velocities, res_out, iter_out);
   
   for(int j = 0; j < nj; ++j)
      for(int i = 0; i < ni+1; ++i)
         if(u_state(i,j) == FLUID)
            u(i,j) = (float)velocities[u_ind(i,j)];
         else if(u_state(i,j) == SOLID) 
            u(i,j) = u_obj;
         

   
   for(int j = 0; j < nj+1; ++j)
      for(int i = 0; i < ni; ++i)
         if(v_state(i,j) == FLUID)
            v(i,j) = (float)velocities[v_ind(i,j)];
         else if(v_state(i,j) == SOLID) 
            v(i,j) = v_obj;
}




//Apply several iterations of a very simple "Jacobi"-style propagation of valid velocity data in all directions
void extrapolate(Array2f& grid, Array2c& valid) {
   
   Array2c old_valid(valid.ni,valid.nj);
   for(int layers = 0; layers < 10; ++layers) {
      old_valid = valid;
      Array2f temp_grid = grid;
      for(int j = 1; j < grid.nj-1; ++j) for(int i = 1; i < grid.ni-1; ++i) {
         float sum = 0;
         int count = 0;
         
         if(!old_valid(i,j)) {
            
            if(old_valid(i+1,j)) {
               sum += grid(i+1,j);\
               ++count;
            }
            if(old_valid(i-1,j)) {
               sum += grid(i-1,j);\
               ++count;
            }
            if(old_valid(i,j+1)) {
               sum += grid(i,j+1);\
               ++count;
            }
            if(old_valid(i,j-1)) {
               sum += grid(i,j-1);\
               ++count;
            }

            //If any of neighbour cells were valid, 
            //assign the cell their average value and tag it as valid
            if(count > 0) {
               temp_grid(i,j) = sum /(float)count;
               valid(i,j) = 1;
            }

         }
      }
      grid = temp_grid;

   }

}
