/*
 * Copyright 2011-2013 Blender Foundation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "util/util_logging.h"

CCL_NAMESPACE_BEGIN

ccl_device void accum_light_contribution(KernelGlobals *kg,
                                         ShaderData *sd,
                                         ShaderData* emission_sd,
                                         LightSample *ls,
                                         ccl_addr_space PathState *state,
                                         Ray *light_ray,
                                         BsdfEval *L_light,
                                         PathRadiance *L,
                                         bool *is_lamp,
                                         float terminate,
                                         float3 throughput,
                                         float scale)
{
	if(direct_emission(kg, sd, emission_sd, ls, state, light_ray, L_light, is_lamp, terminate)) {
		/* trace shadow ray */
		float3 shadow;

		if(!shadow_blocked(kg, sd, emission_sd, state, light_ray, &shadow)) {
			/* accumulate */
			path_radiance_accum_light(L, state, throughput*scale, L_light, shadow, scale, *is_lamp);
		}
		else {
			path_radiance_accum_total_light(L, state, throughput*scale, L_light);
		}
	}
}

/* Decides whether to go down both childen or only one in the tree traversal */
ccl_device bool split(KernelGlobals *kg, float3 P, int node_offset)
{
	/* early exists if never/always splitting */
	const double threshold = (double)kernel_data.integrator.splitting_threshold;
	if(threshold == 0.0){
		return false;
	} else if(threshold == 1.0){
		return true;
	}

	/* extract bounding box of cluster */
	const float4 node1   = kernel_tex_fetch(__light_tree_nodes, node_offset + 1);
	const float4 node2   = kernel_tex_fetch(__light_tree_nodes, node_offset + 2);
	const float3 bboxMin = make_float3( node1[0], node1[1], node1[2]);
	const float3 bboxMax = make_float3( node1[3], node2[0], node2[1]);

	/* if P is inside bounding sphere then split */
	const float3 centroid = 0.5f * (bboxMax + bboxMin);
	const double radius_squared = (double)len_squared(bboxMax - centroid);
	const double dist_squared   = (double)len_squared(centroid - P);
	if(dist_squared <= radius_squared){
		return true;
	}

	/* eq. 8 & 9 */
	/* observed precision issues and issues with overflow of num_emitters_squared.
	 * using doubles to fix this for now. */

	/* Interval the distance can be in: [a,b] */
	const double  radius = sqrt(radius_squared);
	const double  dist   = sqrt(dist_squared);
	const double  a      = dist - radius;
	const double  b      = dist + radius;

	const double g_mean         = 1.0 / (a * b);
	const double g_mean_squared = g_mean * g_mean;
	const double a3             = a * a * a;
	const double b3             = b * b * b;
	const double g_variance     = (b3 - a3) / (3.0 * (b - a) * a3 * b3) -
	                              g_mean_squared;

	/* eq. 10 */
	const float4 node0   = kernel_tex_fetch(__light_tree_nodes, node_offset    );
	const float4 node3   = kernel_tex_fetch(__light_tree_nodes, node_offset + 3);
	const double energy       = (double)node0[0];
	const double e_variance   = (double)node3[3];
	const double num_emitters = (double)__float_as_int(node0[3]);

	const double num_emitters_squared = num_emitters * num_emitters;
	const double e_mean = energy / num_emitters;
	const double e_mean_squared = e_mean * e_mean;
	const double variance = (e_variance * (g_variance + g_mean_squared) +
	                         e_mean_squared * g_variance) * num_emitters_squared;
	/*
	 * If I run into further precision issues
	 *  sigma^2 = (V[e] * (V[g] + E[g]^2) + (E[e]^2 * V[g]) * N^2 =
	 *          = / V[e] = E[e^2] - E[e]^2 =  ((e_1)^2 + (e_2)^2 +..+(e_N)^2)/N - E[e]^2 / =
	 *          = / E[e] = (e_1 + e_2 + .. + e_N)/N / =
	 *            (( ((e_1)^2 + .. +(e_N)^2) / N - (e_1 + .. + e_N)^2 / N^2 )(V[g] + E[g]^2) + V[g](e_1 + .. + e_N)^2 / N^2)N^2 =
	 *          = (  ((e_1)^2 + .. +(e_N)^2) * N - (e_1 + .. + e_N)^2       )(V[g] + E[g]^2) + V[g](e_1 + .. + e_N)^2
	 *
	 * => No need to calculate N^2 which could be really large for the root node when a lot of lights are used
	 * However, e_mean^2 will be really large instead?
	 * */

	/* normalize */
	const double variance_normalized = sqrt(sqrt( 1.0 / (1.0 + sqrt(variance))));

	return variance_normalized < threshold;
}

/* Recursive tree traversal and accumulating contribution to L for each leaf. */
ccl_device void accum_light_tree_contribution(KernelGlobals *kg, float randu,
                                              float randv, int offset,
                                              float pdf_factor, bool can_split,
                                              float3 throughput, PathRadiance *L,
                                              ccl_addr_space PathState * state,
                                              ShaderData *sd, ShaderData *emission_sd,
                                              int *num_lights,
                                              int *num_lights_fail)
{

	float3 P = sd->P;
	float3 N = sd->N;
	float time = sd->time;
	int bounce = state->bounce;

	/* read in first part of node of light BVH */
	int secondChildOffset, distribution_id, num_emitters;
	update_parent_node(kg, offset, &secondChildOffset, &distribution_id, &num_emitters);

	/* Found a leaf - Choose which light to use */
	if(secondChildOffset == -1){ // Found a leaf

		if(num_emitters == 1){
			(*num_lights)++; // used for debugging purposes
			// Distribution_id is the index
			/* consider this as having picked a light. */
			LightSample ls;
			light_point_sample(kg, randu, randv, time, P, bounce, distribution_id, &ls);

			/* combine pdfs */
			ls.pdf *= pdf_factor;

			if(ls.pdf == 0.0f){
				return;
			}


			Ray light_ray;
			BsdfEval L_light;
			bool is_lamp;
			float terminate = path_state_rng_light_termination(kg, state);
			accum_light_contribution(kg, sd, emission_sd, &ls, state,
			                         &light_ray, &L_light, L, &is_lamp,
			                         terminate, throughput, 1.0f);

		} // TODO: do else, i.e. with several lights per node

		return;
	} else { // Interior node, choose which child(ren) to go down

		int child_offsetL = offset + 4;
		int child_offsetR = 4*secondChildOffset;

		/* choose whether to go down both(split) or only one of the children */
		if(can_split && split(kg, P, offset)){
			/* go down both child nodes */
			accum_light_tree_contribution(kg, randu, randv, child_offsetL,
			                              pdf_factor, true, throughput, L,
			                              state, sd, emission_sd, num_lights, num_lights_fail);
			accum_light_tree_contribution(kg, randu, randv, child_offsetR,
			                              pdf_factor, true, throughput, L,
			                              state, sd, emission_sd, num_lights, num_lights_fail);
		} else {
			/* go down one of the child nodes */

			/* calculate probability of going down left node */
			float I_L = calc_node_importance(kg, P, N, child_offsetL);
			float I_R = calc_node_importance(kg, P, N, child_offsetR);

			if( (I_L == 0.0f) && (I_R == 0.0f) ){
				(*num_lights_fail)++; // used for debugging purposes
				return;
			}

			float P_L = I_L / ( I_L + I_R);

			if(randu <= P_L){ // Going down left node
				/* rescale random number */
				randu = randu / P_L;

				offset = child_offsetL;
				pdf_factor *= P_L;
			} else { // Going down right node
				/* rescale random number */
				randu = (randu * (I_L + I_R) - I_L)/I_R;

				offset = child_offsetR;
				pdf_factor *= 1.0f - P_L;
			}

			accum_light_tree_contribution(kg, randu, randv, offset, pdf_factor,
			                              false, throughput, L, state, sd,
			                              emission_sd, num_lights, num_lights_fail);
		}
	}
}

#if defined(__BRANCHED_PATH__) || defined(__SUBSURFACE__) || defined(__SHADOW_TRICKS__) || defined(__BAKING__)
/* branched path tracing: connect path directly to position on one or more lights and add it to L */
ccl_device_noinline void kernel_branched_path_surface_connect_light(
        KernelGlobals *kg,
        ShaderData *sd,
        ShaderData *emission_sd,
        ccl_addr_space PathState *state,
        float3 throughput,
        float num_samples_adjust,
        PathRadiance *L,
        int sample_all_lights)
{
#ifdef __EMISSION__
	/* sample illumination from lights to find path contribution */
	if(!(sd->flag & SD_BSDF_HAS_EVAL))
		return;

	Ray light_ray;
	BsdfEval L_light;
	bool is_lamp;

#  ifdef __OBJECT_MOTION__
	light_ray.time = sd->time;
#  endif

	bool use_light_bvh = kernel_data.integrator.use_light_bvh;
	bool use_splitting = kernel_data.integrator.splitting_threshold != 0.0f;
	if(use_light_bvh && use_splitting){

		int index;
		float randu, randv;
		path_state_rng_2D(kg, state, PRNG_LIGHT_U, &randu, &randv);

		/* sample light group distribution */
		int   group      = light_group_distribution_sample(kg, &randu);
		float group_prob = kernel_tex_fetch(__light_group_sample_prob, group);
		float pdf = 1.0f;
		if(group == LIGHTGROUP_TREE){
			/* accumulate contribution to L from potentially several lights */
			int num_lights = 0;
			int num_lights_fail = 0;
			accum_light_tree_contribution(kg, randu, randv, 0, group_prob, true,
			                              throughput, L, state, sd, emission_sd,
			                              &num_lights, &num_lights_fail);

			/*
			if(num_lights_fail > 1){ // Debug print
				//VLOG(1) << "Sampled " << num_lights << " lights!";
				VLOG(1) << "Traversed " << num_lights_fail << " branches in vain and was able to sample " << num_lights << " though!";
			}
			*/

			/* have accumulated all the contributions so return */
			return;
		} else if(group == LIGHTGROUP_DISTANT) {
			/* pick a single distant light */
			light_distant_sample(kg, sd->P, &randu, &index, &pdf);
		} else if(group == LIGHTGROUP_BACKGROUND) {
			/* pick a single background light */
			light_background_sample(kg, sd->P, &randu, &index, &pdf);
		} else {
			kernel_assert(false);
		}

		/* sample a point on the given distant/background light */
		LightSample ls;
		light_point_sample(kg, randu, randv, sd->time, sd->P, state->bounce, index, &ls);

		/* combine pdfs */
		ls.pdf *= group_prob;

		if(ls.pdf == 0.0f) return;

		/* accumulate the contribution of this distant/background light to L */
		float terminate = path_state_rng_light_termination(kg, state);
		accum_light_contribution(kg, sd, emission_sd, &ls, state, &light_ray,
		                         &L_light, L, &is_lamp, terminate, throughput,
		                         num_samples_adjust);

	} else if(sample_all_lights) {
		/* lamp sampling */
		for(int i = 0; i < kernel_data.integrator.num_all_lights; i++) {
			if(UNLIKELY(light_select_reached_max_bounces(kg, i, state->bounce)))
				continue;

			int num_samples = ceil_to_int(num_samples_adjust*light_select_num_samples(kg, i));
			float num_samples_inv = num_samples_adjust/num_samples;
			uint lamp_rng_hash = cmj_hash(state->rng_hash, i);

			for(int j = 0; j < num_samples; j++) {
				float light_u, light_v;
				path_branched_rng_2D(kg, lamp_rng_hash, state, j, num_samples, PRNG_LIGHT_U, &light_u, &light_v);
				float terminate = path_branched_rng_light_termination(kg, lamp_rng_hash, state, j, num_samples);

				LightSample ls;
				if(lamp_light_sample(kg, i, light_u, light_v, sd->P, &ls)) {
					accum_light_contribution(kg, sd, emission_sd, &ls, state,
					                         &light_ray, &L_light, L, &is_lamp,
					                         terminate, throughput,
					                         num_samples_inv);
				}
			}
		}

		/* mesh light sampling */
		if(kernel_data.integrator.pdf_triangles != 0.0f) {
			int num_samples = ceil_to_int(num_samples_adjust*kernel_data.integrator.mesh_light_samples);
			float num_samples_inv = num_samples_adjust/num_samples;

			for(int j = 0; j < num_samples; j++) {
				float light_u, light_v;
				path_branched_rng_2D(kg, state->rng_hash, state, j, num_samples, PRNG_LIGHT_U, &light_u, &light_v);
				float terminate = path_branched_rng_light_termination(kg, state->rng_hash, state, j, num_samples);

				/* only sample triangle lights */
				if(kernel_data.integrator.num_all_lights)
					light_u = 0.5f*light_u;

				kernel_assert(!kernel_data.integrator.use_light_bvh);

				LightSample ls;
				if(light_sample(kg, light_u, light_v, sd->time, sd->P, sd->N, state->bounce, &ls)) {
					/* Same as above, probability needs to be corrected since the sampling was forced to select a mesh light. */
					if(kernel_data.integrator.num_all_lights)
						ls.pdf *= 2.0f;

					accum_light_contribution(kg, sd, emission_sd, &ls, state,
					                         &light_ray, &L_light, L, &is_lamp,
					                         terminate, throughput, num_samples_inv);
				}
			}
		}
	}
	else {
		/* sample one light at random */
		float light_u, light_v;
		path_state_rng_2D(kg, state, PRNG_LIGHT_U, &light_u, &light_v);
		float terminate = path_state_rng_light_termination(kg, state);

		LightSample ls;
		if(light_sample(kg, light_u, light_v, sd->time, sd->P, sd->N, state->bounce, &ls)) {
			/* sample random light */
			accum_light_contribution(kg, sd, emission_sd, &ls, state, &light_ray,
			                         &L_light, L, &is_lamp, terminate, throughput,
			                         num_samples_adjust);
		}
	}
#endif
}

/* branched path tracing: bounce off or through surface to with new direction stored in ray */
ccl_device bool kernel_branched_path_surface_bounce(
        KernelGlobals *kg,
        ShaderData *sd,
        const ShaderClosure *sc,
        int sample,
        int num_samples,
        ccl_addr_space float3 *throughput,
        ccl_addr_space PathState *state,
        PathRadianceState *L_state,
        ccl_addr_space Ray *ray,
        float sum_sample_weight)
{
	/* sample BSDF */
	float bsdf_pdf;
	BsdfEval bsdf_eval;
	float3 bsdf_omega_in;
	differential3 bsdf_domega_in;
	float bsdf_u, bsdf_v;
	path_branched_rng_2D(kg, state->rng_hash, state, sample, num_samples, PRNG_BSDF_U, &bsdf_u, &bsdf_v);
	int label;

	label = shader_bsdf_sample_closure(kg, sd, sc, bsdf_u, bsdf_v, &bsdf_eval,
		&bsdf_omega_in, &bsdf_domega_in, &bsdf_pdf);

	if(bsdf_pdf == 0.0f || bsdf_eval_is_zero(&bsdf_eval))
		return false;

	/* modify throughput */
	path_radiance_bsdf_bounce(kg, L_state, throughput, &bsdf_eval, bsdf_pdf, state->bounce, label);

#ifdef __DENOISING_FEATURES__
	state->denoising_feature_weight *= sc->sample_weight / (sum_sample_weight * num_samples);
#endif

	/* modify path state */
	path_state_next(kg, state, label);

	/* setup ray */
	ray->P = ray_offset(sd->P, (label & LABEL_TRANSMIT)? -sd->Ng: sd->Ng);
	ray->D = normalize(bsdf_omega_in);
	ray->t = FLT_MAX;
#ifdef __RAY_DIFFERENTIALS__
	ray->dP = sd->dP;
	ray->dD = bsdf_domega_in;
#endif
#ifdef __OBJECT_MOTION__
	ray->time = sd->time;
#endif

#ifdef __VOLUME__
	/* enter/exit volume */
	if(label & LABEL_TRANSMIT)
		kernel_volume_stack_enter_exit(kg, sd, state->volume_stack);
#endif

	/* branch RNG state */
	path_state_branch(state, sample, num_samples);

	/* set MIS state */
	state->min_ray_pdf = fminf(bsdf_pdf, FLT_MAX);
	state->ray_pdf = bsdf_pdf;
#ifdef __LAMP_MIS__
	state->ray_t = 0.0f;
#endif

	return true;
}

#endif

/* path tracing: connect path directly to position on a light and add it to L */
ccl_device_inline void kernel_path_surface_connect_light(KernelGlobals *kg,
	ShaderData *sd, ShaderData *emission_sd, float3 throughput, ccl_addr_space PathState *state,
	PathRadiance *L)
{
#ifdef __EMISSION__
	if(!(kernel_data.integrator.use_direct_light && (sd->flag & SD_BSDF_HAS_EVAL)))
		return;

#ifdef __SHADOW_TRICKS__
	if(state->flag & PATH_RAY_SHADOW_CATCHER) {
		kernel_branched_path_surface_connect_light(kg,
		                                           sd,
		                                           emission_sd,
		                                           state,
		                                           throughput,
		                                           1.0f,
		                                           L,
		                                           1);
		return;
	}
#endif

	/* sample illumination from lights to find path contribution */
	float light_u, light_v;
	path_state_rng_2D(kg, state, PRNG_LIGHT_U, &light_u, &light_v);

	Ray light_ray;
	BsdfEval L_light;
	bool is_lamp;

#ifdef __OBJECT_MOTION__
	light_ray.time = sd->time;
#endif

	LightSample ls;
	if(light_sample(kg, light_u, light_v, sd->time, sd->P, sd->N, state->bounce, &ls)) {
		float terminate = path_state_rng_light_termination(kg, state);
		accum_light_contribution(kg, sd, emission_sd, &ls, state, &light_ray,
		                         &L_light, L, &is_lamp, terminate, throughput,
		                         1.0f);
	}
#endif
}

/* path tracing: bounce off or through surface to with new direction stored in ray */
ccl_device bool kernel_path_surface_bounce(KernelGlobals *kg,
                                           ShaderData *sd,
                                           ccl_addr_space float3 *throughput,
                                           ccl_addr_space PathState *state,
                                           PathRadianceState *L_state,
                                           ccl_addr_space Ray *ray)
{
	/* no BSDF? we can stop here */
	if(sd->flag & SD_BSDF) {
		/* sample BSDF */
		float bsdf_pdf;
		BsdfEval bsdf_eval;
		float3 bsdf_omega_in;
		differential3 bsdf_domega_in;
		float bsdf_u, bsdf_v;
		path_state_rng_2D(kg, state, PRNG_BSDF_U, &bsdf_u, &bsdf_v);
		int label;

		label = shader_bsdf_sample(kg, sd, bsdf_u, bsdf_v, &bsdf_eval,
			&bsdf_omega_in, &bsdf_domega_in, &bsdf_pdf);

		if(bsdf_pdf == 0.0f || bsdf_eval_is_zero(&bsdf_eval))
			return false;

		/* modify throughput */
		path_radiance_bsdf_bounce(kg, L_state, throughput, &bsdf_eval, bsdf_pdf, state->bounce, label);

		/* set labels */
		if(!(label & LABEL_TRANSPARENT)) {
			state->ray_pdf = bsdf_pdf;
#ifdef __LAMP_MIS__
			state->ray_t = 0.0f;
#endif
			state->min_ray_pdf = fminf(bsdf_pdf, state->min_ray_pdf);
		}

		/* update path state */
		path_state_next(kg, state, label);

		/* setup ray */
		ray->P = ray_offset(sd->P, (label & LABEL_TRANSMIT)? -sd->Ng: sd->Ng);
		ray->D = normalize(bsdf_omega_in);

		if(state->bounce == 0)
			ray->t -= sd->ray_length; /* clipping works through transparent */
		else
			ray->t = FLT_MAX;

#ifdef __RAY_DIFFERENTIALS__
		ray->dP = sd->dP;
		ray->dD = bsdf_domega_in;
#endif

#ifdef __VOLUME__
		/* enter/exit volume */
		if(label & LABEL_TRANSMIT)
			kernel_volume_stack_enter_exit(kg, sd, state->volume_stack);
#endif
		return true;
	}
#ifdef __VOLUME__
	else if(sd->flag & SD_HAS_ONLY_VOLUME) {
		if(!path_state_volume_next(kg, state)) {
			return false;
		}

		if(state->bounce == 0)
			ray->t -= sd->ray_length; /* clipping works through transparent */
		else
			ray->t = FLT_MAX;

		/* setup ray position, direction stays unchanged */
		ray->P = ray_offset(sd->P, -sd->Ng);
#ifdef __RAY_DIFFERENTIALS__
		ray->dP = sd->dP;
#endif

		/* enter/exit volume */
		kernel_volume_stack_enter_exit(kg, sd, state->volume_stack);
		return true;
	}
#endif
	else {
		/* no bsdf or volume? */
		return false;
	}
}

CCL_NAMESPACE_END

