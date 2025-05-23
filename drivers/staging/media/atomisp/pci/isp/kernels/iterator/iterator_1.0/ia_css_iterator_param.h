/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Support for Intel Camera Imaging ISP subsystem.
 * Copyright (c) 2015, Intel Corporation.
 */

#ifndef __IA_CSS_ITERATOR_PARAM_H
#define __IA_CSS_ITERATOR_PARAM_H

#include "ia_css_types.h" /* ia_css_resolution */
#include "ia_css_frame_public.h" /* ia_css_frame_info */
#include "ia_css_frame_comm.h" /* ia_css_frame_sp_info */

struct ia_css_iterator_configuration {
	const struct ia_css_frame_info *input_info;
	const struct ia_css_frame_info *internal_info;
	const struct ia_css_frame_info *output_info;
	const struct ia_css_frame_info *vf_info;
	const struct ia_css_resolution *dvs_envelope;
};

struct sh_css_isp_iterator_isp_config {
	struct ia_css_frame_sp_info input_info;
	struct ia_css_frame_sp_info internal_info;
	struct ia_css_frame_sp_info output_info;
	struct ia_css_frame_sp_info vf_info;
	struct ia_css_sp_resolution dvs_envelope;
};

#endif /* __IA_CSS_ITERATOR_PARAM_H */
