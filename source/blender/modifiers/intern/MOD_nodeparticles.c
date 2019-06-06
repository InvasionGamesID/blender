/*
 * ***** BEGIN GPL LICENSE BLOCK *****
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software  Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * The Original Code is Copyright (C) 2019 by the Blender Foundation.
 * All rights reserved.
 * ***** END GPL LICENSE BLOCK *****
 *
 */

/** \file blender/modifiers/intern/MOD_nodeparticles.c
 *  \ingroup modifiers
 *
 */

#include "MEM_guardedalloc.h"

#include "DNA_mesh_types.h"
#include "DNA_meshdata_types.h"
#include "DNA_modifier_types.h"
#include "DNA_object_types.h"

#include "BKE_mesh.h"
#include "BKE_modifier.h"

#include "BLI_math.h"

#include "MOD_util.h"

#include "SIM_particles.h"

typedef struct RuntimeData {
  ParticleSystemRef particle_system;
  ParticlesStateRef state;
} RuntimeData;

static RuntimeData *get_runtime_data(NodeParticlesModifierData *npmd)
{
  RuntimeData *data = npmd->modifier.runtime;
  BLI_assert(data);
  return data;
}

static void ensure_runtime_data(NodeParticlesModifierData *npmd)
{
  if (npmd->modifier.runtime != NULL) {
    return;
  }

  RuntimeData *data = MEM_callocN(sizeof(RuntimeData), __func__);
  data->particle_system = SIM_particle_system_new();
  data->state = SIM_particles_state_new(data->particle_system);
  npmd->modifier.runtime = data;
}

static void free_runtime_data(RuntimeData *npmd)
{
  SIM_particles_state_free(npmd->state);
  SIM_particle_system_free(npmd->particle_system);
  MEM_freeN(npmd);
}

static void clear_runtime_data(NodeParticlesModifierData *npmd)
{
  if (npmd->modifier.runtime != NULL) {
    free_runtime_data(npmd->modifier.runtime);
    npmd->modifier.runtime = NULL;
  }
}

static Mesh *applyModifier(ModifierData *md,
                           const struct ModifierEvalContext *UNUSED(ctx),
                           Mesh *UNUSED(mesh))
{
  NodeParticlesModifierData *npmd = (NodeParticlesModifierData *)md;
  ensure_runtime_data(npmd);
  RuntimeData *runtime = get_runtime_data(npmd);

  SIM_particle_system_step(runtime->state);

  uint point_amount = SIM_particles_count(runtime->state);
  Mesh *mesh = BKE_mesh_new_nomain(point_amount, 0, 0, 0, 0);

  float(*positions)[3] = MEM_malloc_arrayN(point_amount, sizeof(float[3]), __func__);
  SIM_particles_get_positions(runtime->state, positions);

  for (uint i = 0; i < point_amount; i++) {
    copy_v3_v3(mesh->mvert[i].co, positions[i]);
  }

  MEM_freeN(positions);

  return mesh;
}

static void initData(ModifierData *UNUSED(md))
{
}

static void freeData(ModifierData *md)
{
  NodeParticlesModifierData *npmd = (NodeParticlesModifierData *)md;
  clear_runtime_data(npmd);
}

static void freeRuntimeData(void *runtime_data_v)
{
  if (runtime_data_v == NULL) {
    return;
  }
  RuntimeData *data = (RuntimeData *)runtime_data_v;
  free_runtime_data(data);
}

static bool dependsOnTime(ModifierData *UNUSED(md))
{
  return true;
}

static void updateDepsgraph(ModifierData *UNUSED(md),
                            const ModifierUpdateDepsgraphContext *UNUSED(ctx))
{
}

static void foreachIDLink(ModifierData *UNUSED(md),
                          Object *UNUSED(ob),
                          IDWalkFunc UNUSED(walk),
                          void *UNUSED(userData))
{
}

ModifierTypeInfo modifierType_NodeParticles = {
    /* name */ "Node Particles",
    /* structName */ "NodeParticlesModifierData",
    /* structSize */ sizeof(NodeParticlesModifierData),
    /* type */ eModifierTypeType_Constructive,
    /* flags */ eModifierTypeFlag_AcceptsMesh,
    /* copyData */ modifier_copyData_generic,

    /* deformVerts */ NULL,
    /* deformMatrices */ NULL,
    /* deformVertsEM */ NULL,
    /* deformMatricesEM */ NULL,
    /* applyModifier */ applyModifier,

    /* initData */ initData,
    /* requiredDataMask */ NULL,
    /* freeData */ freeData,
    /* isDisabled */ NULL,
    /* updateDepsgraph */ updateDepsgraph,
    /* dependsOnTime */ dependsOnTime,
    /* dependsOnNormals */ NULL,
    /* foreachObjectLink */ NULL,
    /* foreachIDLink */ foreachIDLink,
    /* foreachTexLink */ NULL,
    /* freeRuntimeData */ freeRuntimeData,
};
