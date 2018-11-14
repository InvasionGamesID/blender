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
 * along with this program; if not, write to the Free Software Foundation,
 * Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 *
 * Contributor(s): Chingiz Dyussenov, Arystanbek Dyussenov, Jan Diederich, Tod Liverseed.
 *
 * ***** END GPL LICENSE BLOCK *****
 */

/** \file AnimationExporter.cpp
 *  \ingroup collada
 */

#include "GeometryExporter.h"
#include "AnimationExporter.h"
#include "AnimationClipExporter.h"
#include "BCAnimationSampler.h"
#include "MaterialExporter.h"
#include "collada_utils.h"

std::string EMPTY_STRING;

std::string AnimationExporter::get_axis_name(std::string channel, int id)
{
	static std::map<std::string, std::vector<std::string>> BC_COLLADA_AXIS_FROM_TYPE = {
		{ "color"         ,{ "R", "G", "B" } },
		{ "specular_color",{ "R", "G", "B" } },
		{ "diffuse_color",{ "R", "G", "B" } },
		{ "alpha",{ "R", "G", "B" } },
		{ "scale",{ "X", "Y", "Z" } },
		{ "location",{ "X", "Y", "Z" } },
		{ "rotation_euler",{ "X", "Y", "Z" } }
	};

	std::map<std::string, std::vector<std::string>>::const_iterator it;
	it = BC_COLLADA_AXIS_FROM_TYPE.find(channel);
	if (it == BC_COLLADA_AXIS_FROM_TYPE.end())
		return "";

	const std::vector<std::string> &subchannel = it->second;
	if (id >= subchannel.size())
		return "";
	return subchannel[id];
}

bool AnimationExporter::open_animation_container(bool has_container, Object *ob)
{
	if (!has_container) {
		char anim_id[200];
		sprintf(anim_id, "action_container-%s", translate_id(id_name(ob)).c_str());
		openAnimation(anim_id, encode_xml(id_name(ob)));
	}
	return true;
}

void AnimationExporter::openAnimationWithClip(std::string action_id, std::string action_name)
{
	std::vector<std::string> anim_meta_entry;
	anim_meta_entry.push_back(translate_id(action_id));
	anim_meta_entry.push_back(action_name);
	anim_meta.push_back(anim_meta_entry);

	openAnimation(translate_id(action_id), action_name);
}

void AnimationExporter::close_animation_container(bool has_container)
{
	if (has_container)
		closeAnimation();
}

bool AnimationExporter::exportAnimations(Main *bmain, Scene *sce)
{
	LinkNode &export_set = *this->export_settings->export_set;
	bool has_anim_data = bc_has_animations(sce, export_set);
	int animation_count = 0;
	if (has_anim_data) {

		this->scene = sce;

		BCObjectSet animated_subset;
		BCAnimationSampler::get_animated_from_export_set(animated_subset, export_set);
		animation_count = animated_subset.size();
		BCAnimationSampler animation_sampler(depsgraph, mContext, animated_subset);

		try {
			animation_sampler.sample_scene(scene,
				export_settings->sampling_rate,
				/*keyframe_at_end = */ true,
				export_settings->open_sim,
				export_settings->keep_keyframes,
				export_settings->export_animation_type
			);

			openLibrary();

			BCObjectSet::iterator it;
			for (it = animated_subset.begin(); it != animated_subset.end(); ++it) {
				Object *ob = *it;
				exportAnimation(ob, animation_sampler);
			}
		}
		catch (std::invalid_argument &iae)
		{
			fprintf(stderr, "Animation export interrupted");
			fprintf(stderr, "Exception was: %s", iae.what());
		}

		closeLibrary();

#if 0
		/* TODO: If all actions shall be exported, we need to call the
		 * AnimationClipExporter which will figure out which actions
		 * need to be exported for which objects
		 */ 
		if (this->export_settings->include_all_actions) {
			AnimationClipExporter ace(eval_ctx, sw, export_settings, anim_meta);
			ace.exportAnimationClips(sce);
		}
#endif
	}
	return animation_count;
}

/* called for each exported object */
void AnimationExporter::exportAnimation(Object *ob, BCAnimationSampler &sampler)
{
	bool container_is_open = false;

	//Transform animations (trans, rot, scale)
	container_is_open = open_animation_container(container_is_open, ob);

	/* Now take care of the Object Animations
	 * Note: For Armatures the skeletal animation has already been exported (see above)
	 * However Armatures also can have Object animation.
	 */
	bool export_as_matrix = this->export_settings->export_transformation_type == BC_TRANSFORMATION_TYPE_MATRIX;
	if (export_as_matrix) {
		export_matrix_animation(ob, sampler); // export all transform_curves as one single matrix animation
	}

	export_curve_animation_set(ob, sampler, export_as_matrix);

	if (ob->type == OB_ARMATURE) {

#ifdef WITH_MORPH_ANIMATION
		/* TODO: This needs to be handled by extra profiles, postponed for now */
		export_morph_animation(ob);
#endif

		/* Export skeletal animation (if any) */
		bArmature *arm = (bArmature *)ob->data;
		for (Bone *root_bone = (Bone *)arm->bonebase.first; root_bone; root_bone = root_bone->next)
			export_bone_animations_recursive(ob, root_bone, sampler);
	}

	close_animation_container(container_is_open);
}

/*
 * Export all animation FCurves of an Object.
 *
 * Note: This uses the keyframes as sample points,
 * and exports "baked keyframes" while keeping the tangent information
 * of the FCurves intact. This works for simple cases, but breaks
 * especially when negative scales are involved in the animation.
 * And when parent inverse matrices are involved (when exporting
 * object hierarchies)
 *
 */
void AnimationExporter::export_curve_animation_set(Object *ob, BCAnimationSampler &sampler, bool export_as_matrix)
{
	BCAnimationCurveMap *curves = sampler.get_curves(ob);

	BCAnimationCurveMap::iterator it;
	for (it = curves->begin(); it != curves->end(); ++it) {
		BCAnimationCurve &curve = *it->second;
		if (curve.get_channel_target() == "rotation_quaternion") {
			/*
			   Can not export Quaternion animation in Collada as far as i know)
			   Maybe automatically convert to euler rotation?
			   Discard for now.
			*/
			continue;
		}

		if (export_as_matrix && curve.is_transform_curve()) {
			/* All Transform curves will be exported within a single matrix animation,
			 * see export_matrix_animation()
			 * No need to export the curves here again.
			 */
			continue;
		}

		if (!curve.is_animated()) {
			continue;
		}

		BCAnimationCurve *mcurve = get_modified_export_curve(ob, curve, *curves);
		if (mcurve) {
			export_curve_animation(ob, *mcurve);
			delete mcurve;
		}
		else {
			export_curve_animation(ob, curve);
		}
	}
}

void AnimationExporter::export_matrix_animation(Object *ob, BCAnimationSampler &sampler)
{
	std::vector<float> frames;
	sampler.get_object_frames(frames, ob);
	if (frames.size() > 0) {
		BCMatrixSampleMap samples;
		bool is_animated = sampler.get_object_samples(samples, ob);
		if (is_animated) {
			bAction *action = bc_getSceneObjectAction(ob);
			std::string name = encode_xml(id_name(ob));
			std::string action_name = (action == NULL) ? name + "-action" : id_name(action);
			std::string channel_type = "transform";
			std::string axis = "";
			std::string id = bc_get_action_id(action_name, name, channel_type, axis);

			std::string target = translate_id(name) + '/' + channel_type;

			export_collada_matrix_animation(id, name, target, frames, samples);
		}
	}
}

//write bone animations in transform matrix sources
void AnimationExporter::export_bone_animations_recursive(Object *ob, Bone *bone, BCAnimationSampler &sampler)
{
	std::vector<float> frames;
	sampler.get_bone_frames(frames, ob, bone);
	
	if (frames.size()) {
		BCMatrixSampleMap samples;
		bool is_animated = sampler.get_bone_samples(samples, ob, bone);
		if (is_animated) {
			export_bone_animation(ob, bone, frames, samples);
		}
	}

	for (Bone *child = (Bone *)bone->childbase.first; child; child = child->next)
		export_bone_animations_recursive(ob, child, sampler);
}

/*
* In some special cases the exported Curve needs to be replaced
* by a modified curve (for collada purposes)
* This method checks if a conversion is necessary and if applicable
* returns a pointer to the modified BCAnimationCurve.
* IMPORTANT: the modified curve must be deleted by the caller when no longer needed
* if no conversion is needed this method returns a NULL;
*/
BCAnimationCurve *AnimationExporter::get_modified_export_curve(Object *ob, BCAnimationCurve &curve, BCAnimationCurveMap &curves)
{
	std::string channel_target = curve.get_channel_target();
	BCAnimationCurve *mcurve = NULL;
	if (channel_target == "lens") {

		/* Create an xfov curve */

		BCCurveKey key(BC_ANIMATION_TYPE_CAMERA, "xfov", 0);
		mcurve = new BCAnimationCurve(key, ob);

		// now tricky part: transform the fcurve
		BCValueMap lens_values;
		curve.get_value_map(lens_values);

		BCAnimationCurve *sensor_curve = NULL;
		BCCurveKey sensor_key(BC_ANIMATION_TYPE_CAMERA, "sensor_width", 0);
		BCAnimationCurveMap::iterator cit = curves.find(sensor_key);
		if (cit != curves.end()) {
			sensor_curve = cit->second;
		}

		BCValueMap::const_iterator vit;
		for (vit = lens_values.begin(); vit != lens_values.end(); ++vit) {
			int frame = vit->first;
			float lens_value = vit->second;

			float sensor_value;
			if (sensor_curve) {
				sensor_value = sensor_curve->get_value(frame);
			}
			else {
				sensor_value = ((Camera *)ob->data)->sensor_x;
			}
			float value = RAD2DEGF(focallength_to_fov(lens_value, sensor_value));
			mcurve->add_value(value, frame);
		}
		mcurve->clean_handles(); // to reset the handles
	}
	return mcurve;
}

void AnimationExporter::export_curve_animation(
	Object *ob,
	BCAnimationCurve &curve)
{
	std::string channel_target = curve.get_channel_target();

	/*
	 * Some curves can not be exported as is and need some conversion
	 * For more information see implementation oif get_modified_export_curve()
	 * note: if mcurve is not NULL then it must be deleted at end of this method;
	 */

	int channel_index = curve.get_channel_index();
	std::string axis = get_axis_name(channel_target, channel_index); // RGB or XYZ or ""

	std::string action_name;
	bAction *action = bc_getSceneObjectAction(ob);
	action_name = (action) ? id_name(action) : "constraint_anim";

	const std::string curve_name = encode_xml(curve.get_animation_name(ob));
	std::string id = bc_get_action_id(action_name, curve_name, channel_target, axis, ".");

	std::string collada_target = translate_id(curve_name);

	if (curve.is_of_animation_type(BC_ANIMATION_TYPE_MATERIAL)) {
		int material_index = curve.get_subindex();
		Material *ma = give_current_material(ob, material_index + 1);
		if (ma) {
			collada_target = translate_id(id_name(ma)) + "-effect/common/" + get_collada_sid(curve, axis);
		}
	}
	else {
		collada_target += "/" + get_collada_sid(curve, axis);
	}

	export_collada_curve_animation(id, curve_name, collada_target, axis, curve);

}

void AnimationExporter::export_bone_animation(Object *ob, Bone *bone, BCFrames &frames, BCMatrixSampleMap &samples)
{
	bAction* action = bc_getSceneObjectAction(ob);
	std::string bone_name(bone->name);
	std::string name = encode_xml(id_name(ob));
	std::string id = bc_get_action_id(id_name(action), name, bone_name, "pose_matrix");
	std::string target = translate_id(id_name(ob) + "_" + bone_name) + "/transform";

	export_collada_matrix_animation(id, name, target, frames, samples);
}

bool AnimationExporter::is_bone_deform_group(Bone *bone)
{
	bool is_def;
	//Check if current bone is deform
	if ((bone->flag & BONE_NO_DEFORM) == 0) return true;
	//Check child bones
	else {
		for (Bone *child = (Bone *)bone->childbase.first; child; child = child->next) {
			//loop through all the children until deform bone is found, and then return
			is_def = is_bone_deform_group(child);
			if (is_def) return true;
		}
	}
	//no deform bone found in children also
	return false;
}


void AnimationExporter::export_collada_curve_animation(
	std::string id,
	std::string name,
	std::string collada_target,
	std::string axis,
	BCAnimationCurve &curve)
{
	BCFrames frames;
	BCValues values;
	curve.get_frames(frames);
	curve.get_values(values);
	std::string channel_target = curve.get_channel_target();

	fprintf(stdout, "Export animation curve %s (%d control points)\n", id.c_str(), int(frames.size()));
	openAnimation(id, name);
	BC_animation_source_type source_type = (curve.is_rotation_curve()) ? BC_SOURCE_TYPE_ANGLE : BC_SOURCE_TYPE_VALUE;

	std::string input_id = collada_source_from_values(BC_SOURCE_TYPE_TIMEFRAME, COLLADASW::InputSemantic::INPUT, frames, id, axis);
	std::string output_id = collada_source_from_values(source_type, COLLADASW::InputSemantic::OUTPUT, values, id, axis);

	bool has_tangents = false;
	std::string interpolation_id;
	if (this->export_settings->keep_smooth_curves)
		interpolation_id = collada_interpolation_source(curve, id, axis, &has_tangents);
	else
		interpolation_id = collada_linear_interpolation_source(frames.size(), id);

	std::string intangent_id;
	std::string outtangent_id;
	if (has_tangents) {
		intangent_id = collada_tangent_from_curve(COLLADASW::InputSemantic::IN_TANGENT, curve, id, axis);
		outtangent_id = collada_tangent_from_curve(COLLADASW::InputSemantic::OUT_TANGENT, curve, id, axis);
	}

	std::string sampler_id = std::string(id) + SAMPLER_ID_SUFFIX;

	COLLADASW::LibraryAnimations::Sampler sampler(sw, sampler_id);

	sampler.addInput(COLLADASW::InputSemantic::INPUT, COLLADABU::URI(EMPTY_STRING, input_id));
	sampler.addInput(COLLADASW::InputSemantic::OUTPUT, COLLADABU::URI(EMPTY_STRING, output_id));
	sampler.addInput(COLLADASW::InputSemantic::INTERPOLATION, COLLADABU::URI(EMPTY_STRING, interpolation_id));

	if (has_tangents) {
		sampler.addInput(COLLADASW::InputSemantic::IN_TANGENT, COLLADABU::URI(EMPTY_STRING, intangent_id));
		sampler.addInput(COLLADASW::InputSemantic::OUT_TANGENT, COLLADABU::URI(EMPTY_STRING, outtangent_id));
	}

	addSampler(sampler);
	addChannel(COLLADABU::URI(EMPTY_STRING, sampler_id), collada_target);

	closeAnimation();
}

void AnimationExporter::export_collada_matrix_animation(std::string id, std::string name, std::string target, BCFrames &frames, BCMatrixSampleMap &samples)
{
	fprintf(stdout, "Export animation matrix %s (%d control points)\n", id.c_str(), int(frames.size()));

	openAnimationWithClip(id, name);

	std::string input_id = collada_source_from_values(BC_SOURCE_TYPE_TIMEFRAME, COLLADASW::InputSemantic::INPUT, frames, id, "");
	std::string output_id = collada_source_from_values(samples, id);
	std::string interpolation_id = collada_linear_interpolation_source(frames.size(), id);

	std::string sampler_id = std::string(id) + SAMPLER_ID_SUFFIX;
	COLLADASW::LibraryAnimations::Sampler sampler(sw, sampler_id);


	sampler.addInput(COLLADASW::InputSemantic::INPUT, COLLADABU::URI(EMPTY_STRING, input_id));
	sampler.addInput(COLLADASW::InputSemantic::OUTPUT, COLLADABU::URI(EMPTY_STRING, output_id));
	sampler.addInput(COLLADASW::InputSemantic::INTERPOLATION, COLLADABU::URI(EMPTY_STRING, interpolation_id));

	// Matrix animation has no tangents

	addSampler(sampler);
	addChannel(COLLADABU::URI(EMPTY_STRING, sampler_id), target);

	closeAnimation();
}

std::string AnimationExporter::get_semantic_suffix(COLLADASW::InputSemantic::Semantics semantic)
{
	switch (semantic) {
		case COLLADASW::InputSemantic::INPUT:
			return INPUT_SOURCE_ID_SUFFIX;
		case COLLADASW::InputSemantic::OUTPUT:
			return OUTPUT_SOURCE_ID_SUFFIX;
		case COLLADASW::InputSemantic::INTERPOLATION:
			return INTERPOLATION_SOURCE_ID_SUFFIX;
		case COLLADASW::InputSemantic::IN_TANGENT:
			return INTANGENT_SOURCE_ID_SUFFIX;
		case COLLADASW::InputSemantic::OUT_TANGENT:
			return OUTTANGENT_SOURCE_ID_SUFFIX;
		default:
			break;
	}
	return "";
}

void AnimationExporter::add_source_parameters(COLLADASW::SourceBase::ParameterNameList& param,
	COLLADASW::InputSemantic::Semantics semantic,
	bool is_rot, 
	const std::string axis, 
	bool transform)
{
	switch (semantic) {
		case COLLADASW::InputSemantic::INPUT:
			param.push_back("TIME");
			break;
		case COLLADASW::InputSemantic::OUTPUT:
			if (is_rot) {
				param.push_back("ANGLE");
			}
			else {
				if (axis != "") {
					param.push_back(axis);
				}
				else
				if (transform) {
					param.push_back("TRANSFORM");
				}
				else {     //assumes if axis isn't specified all axises are added
					param.push_back("X");
					param.push_back("Y");
					param.push_back("Z");
				}
			}
			break;
		case COLLADASW::InputSemantic::IN_TANGENT:
		case COLLADASW::InputSemantic::OUT_TANGENT:
			param.push_back("X");
			param.push_back("Y");
			break;
		default:
			break;
	}
}

std::string AnimationExporter::collada_tangent_from_curve(COLLADASW::InputSemantic::Semantics semantic, BCAnimationCurve &curve, const std::string& anim_id, std::string axis_name)
{
	std::string channel = curve.get_channel_target();

	const std::string source_id = anim_id + get_semantic_suffix(semantic);

	bool is_angle = (bc_startswith(channel, "rotation") || channel == "spot_size");

	COLLADASW::FloatSourceF source(mSW);
	source.setId(source_id);
	source.setArrayId(source_id + ARRAY_ID_SUFFIX);
	source.setAccessorCount(curve.sample_count());
	source.setAccessorStride(2);

	COLLADASW::SourceBase::ParameterNameList &param = source.getParameterNameList();
	add_source_parameters(param, semantic, is_angle, axis_name, false);

	source.prepareToAppendValues();

	const FCurve *fcu = curve.get_fcurve();
	int tangent = (semantic == COLLADASW::InputSemantic::IN_TANGENT) ? 0 : 2;

	for (int i = 0; i < fcu->totvert; ++i) {
		BezTriple &bezt = fcu->bezt[i];

		float sampled_time = bezt.vec[tangent][0];
		float sampled_val = bezt.vec[tangent][1];

		if (is_angle) {
			sampled_val = RAD2DEGF(sampled_val);
		}

		source.appendValues(FRA2TIME(sampled_time));
		source.appendValues(sampled_val);

	}
	source.finish();
	return source_id;
}

std::string AnimationExporter::collada_source_from_values(
	BC_animation_source_type source_type,
	COLLADASW::InputSemantic::Semantics semantic,
	std::vector<float> &values,
	const std::string& anim_id,
	const std::string axis_name)
{
	/* T can be float, int or double */

	int stride = 1;
	int entry_count = values.size() / stride;
	std::string source_id = anim_id + get_semantic_suffix(semantic);

	COLLADASW::FloatSourceF source(mSW);
	source.setId(source_id);
	source.setArrayId(source_id + ARRAY_ID_SUFFIX);
	source.setAccessorCount(entry_count);
	source.setAccessorStride(stride);

	COLLADASW::SourceBase::ParameterNameList &param = source.getParameterNameList();
	add_source_parameters(param, semantic, source_type== BC_SOURCE_TYPE_ANGLE, axis_name, false);

	source.prepareToAppendValues();

	for (int i = 0; i < entry_count; i++) {
		float val = values[i];
		switch (source_type) {
		case BC_SOURCE_TYPE_TIMEFRAME:
			val = FRA2TIME(val);
			break;
		case BC_SOURCE_TYPE_ANGLE:
			val = RAD2DEGF(val);
			break;
		default: break;
		}
		source.appendValues(val);
	}

	source.finish();

	return source_id;
}

/*
 * Create a collada matrix source for a set of samples
*/
std::string AnimationExporter::collada_source_from_values(BCMatrixSampleMap &samples, const std::string &anim_id)
{
	COLLADASW::InputSemantic::Semantics semantic = COLLADASW::InputSemantic::OUTPUT;
	std::string source_id = anim_id + get_semantic_suffix(semantic);

	COLLADASW::Float4x4Source source(mSW);
	source.setId(source_id);
	source.setArrayId(source_id + ARRAY_ID_SUFFIX);
	source.setAccessorCount(samples.size());
	source.setAccessorStride(16);

	COLLADASW::SourceBase::ParameterNameList &param = source.getParameterNameList();
	add_source_parameters(param, semantic, false, "", true);

	source.prepareToAppendValues();

	BCMatrixSampleMap::iterator it;
	int precision = (this->export_settings->limit_precision) ? 6 : -1; // could be made configurable
	for (it = samples.begin(); it != samples.end(); it++) {
		const BCMatrix *sample = it->second;
		double daemat[4][4];
		sample->get_matrix(daemat, true, precision);
		source.appendValues(daemat);
	}

	source.finish();
	return source_id;
}

std::string AnimationExporter::collada_interpolation_source(const BCAnimationCurve &curve,
	const std::string& anim_id, 
	const std::string axis,
	bool *has_tangents)
{
	std::string source_id = anim_id + get_semantic_suffix(COLLADASW::InputSemantic::INTERPOLATION);

	COLLADASW::NameSource source(mSW);
	source.setId(source_id);
	source.setArrayId(source_id + ARRAY_ID_SUFFIX);
	source.setAccessorCount(curve.sample_count());
	source.setAccessorStride(1);

	COLLADASW::SourceBase::ParameterNameList &param = source.getParameterNameList();
	param.push_back("INTERPOLATION");

	source.prepareToAppendValues();

	*has_tangents = false;

	std::vector<float>frames;
	curve.get_frames(frames);

	for (unsigned int i = 0; i < curve.sample_count(); i++) {
		float frame = frames[i];
		int ipo = curve.get_interpolation_type(frame);
		if (ipo == BEZT_IPO_BEZ) {
			source.appendValues(BEZIER_NAME);
			*has_tangents = true;
		}
		else if (ipo == BEZT_IPO_CONST) {
			source.appendValues(STEP_NAME);
		}
		else { // BEZT_IPO_LIN
			source.appendValues(LINEAR_NAME);
		}
	}
	// unsupported? -- HERMITE, CARDINAL, BSPLINE, NURBS

	source.finish();

	return source_id;
}

std::string AnimationExporter::collada_linear_interpolation_source(int tot, const std::string& anim_id)
{
	std::string source_id = anim_id + get_semantic_suffix(COLLADASW::InputSemantic::INTERPOLATION);

	COLLADASW::NameSource source(mSW);
	source.setId(source_id);
	source.setArrayId(source_id + ARRAY_ID_SUFFIX);
	source.setAccessorCount(tot);
	source.setAccessorStride(1);

	COLLADASW::SourceBase::ParameterNameList &param = source.getParameterNameList();
	param.push_back("INTERPOLATION");

	source.prepareToAppendValues();

	for (int i = 0; i < tot; i++) {
		source.appendValues(LINEAR_NAME);
	}

	source.finish();

	return source_id;
}

const std::string AnimationExporter::get_collada_name(std::string channel_target) const
{
	/*
	 * Translation table to map FCurve animation types to Collada animation.
	 * Todo: Maybe we can keep the names from the fcurves here instead of
	 * mapping. However this is what i found in the old code. So keep
	 * this map for now.
	 */
	static std::map<std::string, std::string> BC_CHANNEL_BLENDER_TO_COLLADA = {
		{ "rotation", "rotation" },
		{ "rotation_euler", "rotation" },
		{ "rotation_quaternion", "rotation" },
		{ "scale", "scale" },
		{ "location", "location" },

		/* Materials */
		{ "specular_color", "specular" },
		{ "diffuse_color", "diffuse" },
		{ "ior", "index_of_refraction" },
		{ "specular_hardness", "specular_hardness" },
		{ "alpha", "alpha" },

		/* Lamps */
		{ "color", "color" },
		{ "fall_off_angle", "falloff_angle" },
		{ "spot_size", "falloff_angle" },
		{ "fall_off_exponent", "falloff_exponent" },
		{ "spot_blend", "falloff_exponent" },
		{ "blender/blender_dist", "blender/blender_dist" }, // special blender profile (todo: make this more elegant)
		{ "distance", "blender/blender_dist" }, // special blender profile (todo: make this more elegant)

		/* Cameras */
		{ "lens", "xfov" },
		{ "xfov", "xfov" },
		{ "xmag", "xmag" },
		{ "zfar", "zfar" },
		{ "znear", "znear" },
		{ "ortho_scale", "xmag" },
		{ "clip_end", "zfar" },
		{ "clip_start", "znear" }
	};

	std::map<std::string, std::string>::iterator name_it = BC_CHANNEL_BLENDER_TO_COLLADA.find(channel_target);
	if (name_it == BC_CHANNEL_BLENDER_TO_COLLADA.end())
		return "";

	std::string tm_name = name_it->second;
	return tm_name;
}

/*
 * Assign sid of the animated parameter or transform for rotation,
 * axis name is always appended and the value of append_axis is ignored
 */
std::string AnimationExporter::get_collada_sid(const BCAnimationCurve &curve, const std::string axis_name)
{
	std::string channel_target = curve.get_channel_target();
	std::string tm_name = get_collada_name(channel_target);

	bool is_angle = curve.is_rotation_curve();


	if (tm_name.size()) {
		if (is_angle)
			return tm_name + std::string(axis_name) + ".ANGLE";
		else
			if (axis_name != "")
				return tm_name + "." + std::string(axis_name);
			else
				return tm_name;
	}

	return tm_name;
}

#ifdef WITH_MORPH_ANIMATION
/* TODO: This function needs to be implemented similar to the material animation export
So we have to update BCSample for this to work.
*/
void AnimationExporter::export_morph_animation(Object *ob, BCAnimationSampler &sampler)
{
	FCurve *fcu;
	Key *key = BKE_key_from_object(ob);
	if (!key) return;

	if (key->adt && key->adt->action) {
		fcu = (FCurve *)key->adt->action->curves.first;

		while (fcu) {
			BC_animation_transform_type tm_type = get_transform_type(fcu->rna_path);

			create_keyframed_animation(ob, fcu, tm_type, true, sampler);

			fcu = fcu->next;
		}
	}

}
#endif

#if 0
extern std::map<std::string, BC_animation_transform_type> BC_ANIMATION_TYPE_FROM_NAME;

BC_animation_transform_type AnimationExporter::_get_transform_type(std::string path)
{
	BC_animation_transform_type tm_type;
	// when given rna_path, overwrite tm_type from it
	std::string name = bc_string_after(path, '.');
	std::map<std::string, BC_animation_transform_type>::iterator type_it = BC_ANIMATION_TYPE_FROM_NAME.find(name);
	tm_type = (type_it != BC_ANIMATION_TYPE_FROM_NAME.end()) ? type_it->second : BC_ANIMATION_TYPE_UNKNOWN;

	return tm_type;
}

/* Euler sources from quternion sources
* Important: We assume the object has a scene action.
* If it has not, then Blender will die
*/
void AnimationExporter::get_eul_source_for_quat(std::vector<float> &values, Object *ob)
{
<<<<<<< HEAD
	bArmature *arm = (bArmature *)ob_arm->data;
	int flag = arm->flag;
	std::vector<float> fra;
	char prefix[256];

	BLI_snprintf(prefix, sizeof(prefix), "pose.bones[\"%s\"]", bone->name);

	bPoseChannel *pchan = BKE_pose_channel_find_name(ob_arm->pose, bone->name);
	if (!pchan)
		return;
	//Fill frame array with key frame values framed at \param:transform_type
	switch (transform_type) {
		case 0:
			find_rotation_frames(ob_arm, fra, prefix, pchan->rotmode);
			break;
		case 1:
			find_keyframes(ob_arm, fra, prefix, "scale");
			break;
		case 2:
			find_keyframes(ob_arm, fra, prefix, "location");
			break;
		default:
			return;
	}

	// exit rest position
	if (flag & ARM_RESTPOS) {
		arm->flag &= ~ARM_RESTPOS;
		BKE_pose_where_is(depsgraph, scene, ob_arm);
	}
	//v array will hold all values which will be exported.
	if (fra.size()) {
		float *values = (float *)MEM_callocN(sizeof(float) * 3 * fra.size(), "temp. anim frames");
		sample_animation(values, fra, transform_type, bone, ob_arm, pchan);
=======
	bAction *action = bc_getSceneObjectAction(ob);
>>>>>>> collada

	FCurve *fcu = (FCurve *)action->curves.first;
	const int keys = fcu->totvert;
	std::vector<std::vector<float>> quats;
	quats.resize(keys);
	for (int i = 0; i < keys; i++)
		quats[i].resize(4);

	int curve_count = 0;
	while (fcu) {
		std::string transformName = bc_string_after(fcu->rna_path, '.');

		if (transformName == "rotation_quaternion") {
			curve_count += 1;
			for (int i = 0; i < fcu->totvert; i++) {
				std::vector<float> &quat = quats[i];
				quat[fcu->array_index] = fcu->bezt[i].vec[1][1];
			}
			if (curve_count == 4)
				break; /* Quaternion curves can not use more the 4 FCurves!*/
		}
		fcu = fcu->next;
	}

<<<<<<< HEAD
	// restore restpos
	if (flag & ARM_RESTPOS)
		arm->flag = flag;
	BKE_pose_where_is(depsgraph, scene, ob_arm);
}

void AnimationExporter::sample_animation(float *v, std::vector<float> &frames, int type, Bone *bone, Object *ob_arm, bPoseChannel *pchan)
{
	bPoseChannel *parchan = NULL;
	bPose *pose = ob_arm->pose;

	pchan = BKE_pose_channel_find_name(pose, bone->name);

	if (!pchan)
		return;

	parchan = pchan->parent;

	enable_fcurves(ob_arm->adt->action, bone->name);

	std::vector<float>::iterator it;
	for (it = frames.begin(); it != frames.end(); it++) {
		float mat[4][4], ipar[4][4];

		float ctime = BKE_scene_frame_get_from_ctime(scene, *it);


		BKE_animsys_evaluate_animdata(depsgraph, scene, &ob_arm->id, ob_arm->adt, ctime, ADT_RECALC_ANIM);
		BKE_pose_where_is_bone(depsgraph, scene, ob_arm, pchan, ctime, 1);

		// compute bone local mat
		if (bone->parent) {
			invert_m4_m4(ipar, parchan->pose_mat);
			mul_m4_m4m4(mat, ipar, pchan->pose_mat);
		}
		else
			copy_m4_m4(mat, pchan->pose_mat);

		switch (type) {
			case 0:
				mat4_to_eul(v, mat);
				break;
			case 1:
				mat4_to_size(v, mat);
				break;
			case 2:
				copy_v3_v3(v, mat[3]);
				break;
		}
=======
	float feul[3];
	for (int i = 0; i < keys; i++) {
		std::vector<float> &quat = quats[i];
		quat_to_eul(feul, &quat[0]);
>>>>>>> collada

		for (int k = 0; k < 3; k++)
			values.push_back(feul[k]);
	}
}
#endif