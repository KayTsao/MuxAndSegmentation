/*
 *			GPAC - Multimedia Framework C SDK
 *
 *			Authors: Jean Le Feuvre
 *			Copyright (c) Telecom ParisTech 2000-2012
 *					All rights reserved
 *
 *  This file is part of GPAC / mp4box application
 *
 *  GPAC is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU Lesser General Public License as published by
 *  the Free Software Foundation; either version 2, or (at your option)
 *  any later version.
 *
 *  GPAC is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; see the file COPYING.  If not, write to
 *  the Free Software Foundation, 675 Mass Ave, Cambridge, MA 02139, USA.
 *
 */
#include <gpac/download.h>
#include <gpac/network.h>
#include <gpac/utf.h>
#ifndef GPAC_DISABLE_SMGR
#include <gpac/scene_manager.h>
#endif
#ifdef GPAC_DISABLE_ISOM
#error "Cannot compile MP4Box if GPAC is not built with ISO File Format support"
#else
#if defined(WIN32) && !defined(_WIN32_WCE)
#include <io.h>
#include <fcntl.h>
#endif
#include <gpac/media_tools.h>
/*RTP packetizer flags*/
#ifndef GPAC_DISABLE_STREAMING
#include <gpac/ietf.h>
#endif

#ifndef GPAC_DISABLE_MCRYPT
#include <gpac/ismacryp.h>
#endif

#include <gpac/constants.h>
#include <gpac/internal/mpd.h>
#include <time.h>

#define BUFFSIZE	8192
#define DEFAULT_INTERLEAVING_IN_SEC 0.5

/*in fileimport.c*/
#ifndef GPAC_DISABLE_MEDIA_IMPORT
void convert_file_info(char *inName, u32 trackID);
#endif
#ifndef GPAC_DISABLE_ISOM_WRITE
GF_Err import_file(GF_ISOFile *dest, char *inName, u32 import_flags, Double force_fps, u32 frames_per_sample);
GF_Err split_isomedia_file(GF_ISOFile *mp4, Double split_dur, u64 split_size_kb, char *inName, Double interleaving_time, Double chunk_start, Bool adjust_split_end, char *outName, const char *tmpdir);
GF_Err cat_isomedia_file(GF_ISOFile *mp4, char *fileName, u32 import_flags, Double force_fps, u32 frames_per_sample, char *tmp_dir, Bool force_cat, Bool align_timelines, Bool allow_add_in_command);
#if !defined(GPAC_DISABLE_SCENE_ENCODER)
GF_Err EncodeFile(char *in, GF_ISOFile *mp4, GF_SMEncodeOptions *opts, FILE *logs);
GF_Err EncodeFileChunk(char *chunkFile, char *bifs, char *inputContext, char *outputContext, const char *tmpdir);
#endif
GF_ISOFile *package_file(char *file_name, char *fcc, const char *tmpdir, Bool make_wgt);
#endif
Bool keep_sys_tracks = GF_FALSE;
u32 swf_flags = 0;
Float swf_flatten_angle = 0;
s32 laser_resolution = 0;

#ifndef GPAC_DISABLE_ISOM_HINTING

/*
		MP4 File Hinting
*/

void SetupClockReferences(GF_ISOFile *file)
{
	u32 i, count, ocr_id;
	count = gf_isom_get_track_count(file);
	if (count==1) return;
	ocr_id = 0;
	for (i=0; i<count; i++) {
		if (!gf_isom_is_track_in_root_od(file, i+1)) continue;
		ocr_id = gf_isom_get_track_id(file, i+1);
		break;
	}
	/*doesn't look like MP4*/
	if (!ocr_id) return;
	for (i=0; i<count; i++) {
		GF_ESD *esd = gf_isom_get_esd(file, i+1, 1);
		if (esd) {
			esd->OCRESID = ocr_id;
			gf_isom_change_mpeg4_description(file, i+1, 1, esd);
			gf_odf_desc_del((GF_Descriptor *) esd);
		}
	}
}

/*base RTP payload type used (you can specify your own types if needed)*/
#define BASE_PAYT		96

GF_Err HintFile(GF_ISOFile *file, u32 MTUSize, u32 max_ptime, u32 rtp_rate, u32 base_flags, Bool copy_data, Bool interleave, Bool regular_iod, Bool single_group)
{
	GF_ESD *esd;
	GF_InitialObjectDescriptor *iod;
	u32 i, val, res, streamType;
	u32 sl_mode, prev_ocr, single_ocr, nb_done, tot_bw, bw, flags, spec_type;
	GF_Err e;
	char szPayload[30];
	GF_RTPHinter *hinter;
	Bool copy, has_iod, single_av;
	u8 init_payt = BASE_PAYT;
	u32 mtype;
	GF_SDP_IODProfile iod_mode = GF_SDP_IOD_NONE;
	u32 media_group = 0;
	u8 media_prio = 0;

	tot_bw = 0;
	prev_ocr = 0;
	single_ocr = 1;

	has_iod = 1;
	iod = (GF_InitialObjectDescriptor *) gf_isom_get_root_od(file);
	if (!iod) has_iod = 0;
	else {
		if (!gf_list_count(iod->ESDescriptors)) has_iod = 0;
		gf_odf_desc_del((GF_Descriptor *) iod);
	}

	spec_type = gf_isom_guess_specification(file);
	single_av = single_group ? 1 : gf_isom_is_single_av(file);

	/*first make sure we use a systems track as base OCR*/
	for (i=0; i<gf_isom_get_track_count(file); i++) {
		res = gf_isom_get_media_type(file, i+1);
		if ((res==GF_ISOM_MEDIA_SCENE) || (res==GF_ISOM_MEDIA_OD)) {
			if (gf_isom_is_track_in_root_od(file, i+1)) {
				gf_isom_set_default_sync_track(file, i+1);
				break;
			}
		}
	}

	nb_done = 0;
	for (i=0; i<gf_isom_get_track_count(file); i++) {
		sl_mode = base_flags;
		copy = copy_data;
		/*skip emty tracks (mainly MPEG-4 interaction streams...*/
		if (!gf_isom_get_sample_count(file, i+1)) continue;
		if (!gf_isom_is_track_enabled(file, i+1)) {
			fprintf(stderr, "Track ID %d disabled - skipping hint\n", gf_isom_get_track_id(file, i+1) );
			continue;
		}

		mtype = gf_isom_get_media_type(file, i+1);
		switch (mtype) {
		case GF_ISOM_MEDIA_VISUAL:
			if (single_av) {
				media_group = 2;
				media_prio = 2;
			}
			break;
        case GF_ISOM_MEDIA_AUXV:
            if (single_av) {
                media_group = 2;
                media_prio = 3;
            }
            break;
        case GF_ISOM_MEDIA_PICT:
            if (single_av) {
                media_group = 2;
                media_prio = 4;
            }
            break;
		case GF_ISOM_MEDIA_AUDIO:
			if (single_av) {
				media_group = 2;
				media_prio = 1;
			}
			break;
		case GF_ISOM_MEDIA_HINT:
			continue;
		default:
			/*no hinting of systems track on isma*/
			if (spec_type==GF_ISOM_BRAND_ISMA) continue;
		}
		mtype = gf_isom_get_media_subtype(file, i+1, 1);
		if ((mtype==GF_ISOM_SUBTYPE_MPEG4) || (mtype==GF_ISOM_SUBTYPE_MPEG4_CRYP) ) mtype = gf_isom_get_mpeg4_subtype(file, i+1, 1);

		if (!single_av) {
			/*one media per group only (we should prompt user for group selection)*/
			media_group ++;
			media_prio = 1;
		}

		streamType = 0;
		esd = gf_isom_get_esd(file, i+1, 1);
		if (esd) {
			streamType = esd->decoderConfig->streamType;
			if (!prev_ocr) {
				prev_ocr = esd->OCRESID;
				if (!esd->OCRESID) prev_ocr = esd->ESID;
			} else if (esd->OCRESID && prev_ocr != esd->OCRESID) {
				single_ocr = 0;
			}
			/*OD MUST BE WITHOUT REFERENCES*/
			if (streamType==1) copy = 1;
		}
		gf_odf_desc_del((GF_Descriptor *) esd);

		if (!regular_iod && gf_isom_is_track_in_root_od(file, i+1)) {
			/*single AU - check if base64 would fit in ESD (consider 33% overhead of base64), otherwise stream*/
			if (gf_isom_get_sample_count(file, i+1)==1) {
				GF_ISOSample *samp = gf_isom_get_sample(file, i+1, 1, &val);
				if (streamType) {
					res = gf_hinter_can_embbed_data(samp->data, samp->dataLength, streamType);
				} else {
					/*not a system track, we shall hint it*/
					res = 0;
				}
				if (samp) gf_isom_sample_del(&samp);
				if (res) continue;
			}
		}
		if (interleave) sl_mode |= GP_RTP_PCK_USE_INTERLEAVING;

		hinter = gf_hinter_track_new(file, i+1, MTUSize, max_ptime, rtp_rate, sl_mode, init_payt, copy, media_group, media_prio, &e);

		if (!hinter) {
			if (e) {
				fprintf(stderr, "Cannot create hinter (%s)\n", gf_error_to_string(e));
				if (!nb_done) return e;
			}
			continue;
		}
		bw = gf_hinter_track_get_bandwidth(hinter);
		tot_bw += bw;
		flags = gf_hinter_track_get_flags(hinter);

		//set extraction mode for AVC/SVC
		gf_isom_set_nalu_extract_mode(file, i+1, GF_ISOM_NALU_EXTRACT_LAYER_ONLY);

		gf_hinter_track_get_payload_name(hinter, szPayload);
		fprintf(stderr, "Hinting track ID %d - Type \"%s:%s\" (%s) - BW %d kbps\n", gf_isom_get_track_id(file, i+1), gf_4cc_to_str(mtype), gf_4cc_to_str(mtype), szPayload, bw);
		if (flags & GP_RTP_PCK_SYSTEMS_CAROUSEL) fprintf(stderr, "\tMPEG-4 Systems stream carousel enabled\n");
		/*
				if (flags & GP_RTP_PCK_FORCE_MPEG4) fprintf(stderr, "\tMPEG4 transport forced\n");
				if (flags & GP_RTP_PCK_USE_MULTI) fprintf(stderr, "\tRTP aggregation enabled\n");
		*/
		e = gf_hinter_track_process(hinter);

		if (!e) e = gf_hinter_track_finalize(hinter, has_iod);
		gf_hinter_track_del(hinter);

		if (e) {
			fprintf(stderr, "Error while hinting (%s)\n", gf_error_to_string(e));
			if (!nb_done) return e;
		}
		init_payt++;
		nb_done ++;
	}

	if (has_iod) {
		iod_mode = GF_SDP_IOD_ISMA;
		if (regular_iod) iod_mode = GF_SDP_IOD_REGULAR;
	} else {
		iod_mode = GF_SDP_IOD_NONE;
	}
	gf_hinter_finalize(file, iod_mode, tot_bw);

	if (!single_ocr)
		fprintf(stderr, "Warning: at least 2 timelines found in the file\nThis may not be supported by servers/players\n\n");

	return GF_OK;
}

#endif /*GPAC_DISABLE_ISOM_HINTING*/

#if !defined(GPAC_DISABLE_ISOM_WRITE) && !defined(GPAC_DISABLE_AV_PARSERS)

static void check_media_profile(GF_ISOFile *file, u32 track)
{
	u8 PL;
	GF_M4ADecSpecInfo dsi;
	GF_ESD *esd = gf_isom_get_esd(file, track, 1);
	if (!esd) return;

	switch (esd->decoderConfig->streamType) {
	case 0x04:
		PL = gf_isom_get_pl_indication(file, GF_ISOM_PL_VISUAL);
		if (esd->decoderConfig->objectTypeIndication==GPAC_OTI_VIDEO_MPEG4_PART2) {
			GF_M4VDecSpecInfo dsi;
			gf_m4v_get_config(esd->decoderConfig->decoderSpecificInfo->data, esd->decoderConfig->decoderSpecificInfo->dataLength, &dsi);
			if (dsi.VideoPL > PL) gf_isom_set_pl_indication(file, GF_ISOM_PL_VISUAL, dsi.VideoPL);
		} else if ((esd->decoderConfig->objectTypeIndication==GPAC_OTI_VIDEO_AVC) || (esd->decoderConfig->objectTypeIndication==GPAC_OTI_VIDEO_SVC)) {
			gf_isom_set_pl_indication(file, GF_ISOM_PL_VISUAL, 0x15);
		} else if (!PL) {
			gf_isom_set_pl_indication(file, GF_ISOM_PL_VISUAL, 0xFE);
		}
		break;
	case 0x05:
		PL = gf_isom_get_pl_indication(file, GF_ISOM_PL_AUDIO);
		switch (esd->decoderConfig->objectTypeIndication) {
		case GPAC_OTI_AUDIO_AAC_MPEG2_MP:
		case GPAC_OTI_AUDIO_AAC_MPEG2_LCP:
		case GPAC_OTI_AUDIO_AAC_MPEG2_SSRP:
		case GPAC_OTI_AUDIO_AAC_MPEG4:
			gf_m4a_get_config(esd->decoderConfig->decoderSpecificInfo->data, esd->decoderConfig->decoderSpecificInfo->dataLength, &dsi);
			if (dsi.audioPL > PL) gf_isom_set_pl_indication(file, GF_ISOM_PL_AUDIO, dsi.audioPL);
			break;
		default:
			if (!PL) gf_isom_set_pl_indication(file, GF_ISOM_PL_AUDIO, 0xFE);
		}
		break;
	}
	gf_odf_desc_del((GF_Descriptor *) esd);
}
void remove_systems_tracks(GF_ISOFile *file)
{
	u32 i, count;

	count = gf_isom_get_track_count(file);
	if (count==1) return;

	/*force PL rewrite*/
	gf_isom_set_pl_indication(file, GF_ISOM_PL_VISUAL, 0);
	gf_isom_set_pl_indication(file, GF_ISOM_PL_AUDIO, 0);
	gf_isom_set_pl_indication(file, GF_ISOM_PL_OD, 1);	/*the lib always remove IOD when no profiles are specified..*/

	for (i=0; i<gf_isom_get_track_count(file); i++) {
		switch (gf_isom_get_media_type(file, i+1)) {
		case GF_ISOM_MEDIA_VISUAL:
        case GF_ISOM_MEDIA_AUXV:
        case GF_ISOM_MEDIA_PICT:
		case GF_ISOM_MEDIA_AUDIO:
		case GF_ISOM_MEDIA_TEXT:
		case GF_ISOM_MEDIA_SUBT:
			gf_isom_remove_track_from_root_od(file, i+1);
			check_media_profile(file, i+1);
			break;
		/*only remove real systems tracks (eg, delaing with scene description & presentation)
		but keep meta & all unknown tracks*/
		case GF_ISOM_MEDIA_SCENE:
			switch (gf_isom_get_media_subtype(file, i+1, 1)) {
			case GF_ISOM_MEDIA_DIMS:
				gf_isom_remove_track_from_root_od(file, i+1);
				continue;
			default:
				break;
			}
		case GF_ISOM_MEDIA_OD:
		case GF_ISOM_MEDIA_OCR:
		case GF_ISOM_MEDIA_MPEGJ:
			gf_isom_remove_track(file, i+1);
			i--;
			break;
		default:
			break;
		}
	}
	/*none required*/
	if (!gf_isom_get_pl_indication(file, GF_ISOM_PL_AUDIO)) gf_isom_set_pl_indication(file, GF_ISOM_PL_AUDIO, 0xFF);
	if (!gf_isom_get_pl_indication(file, GF_ISOM_PL_VISUAL)) gf_isom_set_pl_indication(file, GF_ISOM_PL_VISUAL, 0xFF);

	gf_isom_set_pl_indication(file, GF_ISOM_PL_OD, 0xFF);
	gf_isom_set_pl_indication(file, GF_ISOM_PL_SCENE, 0xFF);
	gf_isom_set_pl_indication(file, GF_ISOM_PL_GRAPHICS, 0xFF);
	gf_isom_set_pl_indication(file, GF_ISOM_PL_INLINE, 0);
}

#endif /*!defined(GPAC_DISABLE_ISOM_WRITE) && !defined(GPAC_DISABLE_AV_PARSERS)*/

GF_FileType get_file_type_by_ext(char *inName)
{
	GF_FileType type = GF_FILE_TYPE_NOT_SUPPORTED;
	char *ext = strrchr(inName, '.');
	if (ext) {
		char *sep;
		if (!strcmp(ext, ".gz")) ext = strrchr(ext-1, '.');
		ext+=1;
		sep = strchr(ext, '.');
		if (sep) sep[0] = 0;

		if (!stricmp(ext, "mp4") || !stricmp(ext, "3gp") || !stricmp(ext, "mov") || !stricmp(ext, "3g2") || !stricmp(ext, "3gs")) {
			type = GF_FILE_TYPE_ISO_MEDIA;
		} else if (!stricmp(ext, "bt") || !stricmp(ext, "wrl") || !stricmp(ext, "x3dv")) {
			type = GF_FILE_TYPE_BT_WRL_X3DV;
		} else if (!stricmp(ext, "xmt") || !stricmp(ext, "x3d")) {
			type = GF_FILE_TYPE_XMT_X3D;
		} else if (!stricmp(ext, "lsr") || !stricmp(ext, "saf")) {
			type = GF_FILE_TYPE_LSR_SAF;
		} else if (!stricmp(ext, "svg") || !stricmp(ext, "xsr") || !stricmp(ext, "xml")) {
			type = GF_FILE_TYPE_SVG;
		} else if (!stricmp(ext, "swf")) {
			type = GF_FILE_TYPE_SWF;
		} else if (!stricmp(ext, "jp2")) {
			if (sep) sep[0] = '.';
			return GF_FILE_TYPE_NOT_SUPPORTED;
		}
		else type = GF_FILE_TYPE_NOT_SUPPORTED;

		if (sep) sep[0] = '.';
	}


	/*try open file in read mode*/
	if (!type && gf_isom_probe_file(inName)) type = GF_FILE_TYPE_ISO_MEDIA;
	return type;
}

typedef struct
{
	u32 trackID;
	char *line;
} SDPLine;

typedef enum {
	META_ACTION_SET_TYPE			= 0,
	META_ACTION_ADD_ITEM			= 1,
	META_ACTION_REM_ITEM			= 2,
	META_ACTION_SET_PRIMARY_ITEM	= 3,
	META_ACTION_SET_XML				= 4,
	META_ACTION_SET_BINARY_XML		= 5,
	META_ACTION_REM_XML				= 6,
	META_ACTION_DUMP_ITEM			= 7,
	META_ACTION_DUMP_XML			= 8,
	META_ACTION_ADD_IMAGE_ITEM		= 9,
} MetaActionType;

typedef struct
{
	MetaActionType act_type;
	Bool root_meta, use_dref;
	u32 trackID;
	u32 meta_4cc;
	char szPath[GF_MAX_PATH];
	char szName[1024], mime_type[1024], enc_type[1024];
	u32 item_id;
	Bool primary;
	u32 item_type;
	u32 ref_item_id;
	u32 ref_type;
	GF_ImageItemProperties *image_props;
} MetaAction;


typedef enum {
	TSEL_ACTION_SET_PARAM = 0,
	TSEL_ACTION_REMOVE_TSEL = 1,
	TSEL_ACTION_REMOVE_ALL_TSEL_IN_GROUP = 2,
} TSELActionType;

typedef struct
{
	TSELActionType act_type;
	u32 trackID;

	u32 refTrackID;
	u32 criteria[30];
	u32 nb_criteria;
	Bool is_switchGroup;
	u32 switchGroupID;
} TSELAction;

#define CHECK_NEXT_ARG	if (i+1==(u32)argc) { fprintf(stderr, "Missing arg - please check usage\n"); return mp4box_cleanup(1); }

typedef enum {
	TRAC_ACTION_REM_TRACK		= 0,
	TRAC_ACTION_SET_LANGUAGE	= 1,
	TRAC_ACTION_SET_DELAY		= 2,
	TRAC_ACTION_SET_KMS_URI		= 3,
	TRAC_ACTION_SET_PAR			= 4,
	TRAC_ACTION_SET_HANDLER_NAME= 5,
	TRAC_ACTION_ENABLE			= 6,
	TRAC_ACTION_DISABLE			= 7,
	TRAC_ACTION_REFERENCE		= 8,
	TRAC_ACTION_RAW_EXTRACT		= 9,
	TRAC_ACTION_REM_NON_RAP		= 10,
	TRAC_ACTION_SET_KIND		= 11,
	TRAC_ACTION_REM_KIND		= 12,
	TRAC_ACTION_SET_ID			= 13,
	TRAC_ACTION_SET_UDTA		= 14,
	TRAC_ACTION_SWAP_ID			= 15,
} TrackActionType;

typedef struct
{
	TrackActionType act_type;
	u32 trackID;
	char lang[10];
	s32 delay_ms;
	const char *kms;
	const char *hdl_name;
	s32 par_num, par_den;
	u32 dump_type, sample_num;
	char *out_name;
	char *src_name;
	u32 udta_type;
	char *kind_scheme, *kind_value;
	u32 newTrackID;
} TrackAction;

enum
{
	GF_ISOM_CONV_TYPE_ISMA = 1,
	GF_ISOM_CONV_TYPE_ISMA_EX,
	GF_ISOM_CONV_TYPE_3GPP,
	GF_ISOM_CONV_TYPE_IPOD,
	GF_ISOM_CONV_TYPE_PSP
};



GF_DashSegmenterInput *set_dash_input(GF_DashSegmenterInput *dash_inputs, char *name, u32 *nb_dash_inputs)
{
	GF_DashSegmenterInput *di;
	char *sep;
	// skip ./ and ../, and look for first . to figure out extension
	if ((name[1]=='/') || (name[2]=='/') || (name[1]=='\\') || (name[2]=='\\') ) sep = strchr(name+3, '.');
	else {
		char *s2 = strchr(name, ':');
		sep = strchr(name, '.');
		if (sep && s2 && (s2 - sep) < 0) {
			sep = name;
		}
	}

	//then look for our opt separator :
	sep = strchr(sep ? sep : name, ':');

	if (sep && (sep[1]=='\\')) sep = strchr(sep+1, ':');

	dash_inputs = gf_realloc(dash_inputs, sizeof(GF_DashSegmenterInput) * (*nb_dash_inputs + 1) );
	memset(&dash_inputs[*nb_dash_inputs], 0, sizeof(GF_DashSegmenterInput) );
	di = &dash_inputs[*nb_dash_inputs];
	(*nb_dash_inputs)++;

	if (sep) {
		char *opts, *first_opt;
		opts = first_opt = sep;
		while (opts) {
			sep = strchr(opts, ':');
			while (sep) {
				/* this is a real separator if it is followed by a keyword we are looking for */
				if (!strnicmp(sep, ":id=", 4) ||
				        !strnicmp(sep, ":dur=", 5) ||
				        !strnicmp(sep, ":period=", 8) ||
				        !strnicmp(sep, ":BaseURL=", 9) ||
				        !strnicmp(sep, ":bandwidth=", 11) ||
				        !strnicmp(sep, ":role=", 6) ||
				        !strnicmp(sep, ":desc", 5) ||
				        !strnicmp(sep, ":duration=", 10) || /*legacy*/!strnicmp(sep, ":period_duration=", 10) ||
				        !strnicmp(sep, ":xlink=", 7)) {
					break;
				} else {
					sep = strchr(sep+1, ':');
				}
			}
			if (sep && !strncmp(sep, "://", 3)) sep = strchr(sep+3, ':');
			if (sep) sep[0] = 0;

			if (!strnicmp(opts, "id=", 3)) {
				u32 i;
				di->representationID = gf_strdup(opts+3);
				/* check to see if this representation Id has already been assigned */
				for (i=0; i<(*nb_dash_inputs)-1; i++) {
					GF_DashSegmenterInput *other_di;
					other_di = &dash_inputs[i];
					if (!strcmp(other_di->representationID, di->representationID)) {
						GF_LOG(GF_LOG_ERROR, GF_LOG_DASH, ("[DASH] Error: Duplicate Representation ID \"%s\" in command line\n", di->representationID));
					}
				}
			} else if (!strnicmp(opts, "dur=", 4)) di->media_duration = (Double)atof(opts+4);
			else if (!strnicmp(opts, "period=", 7)) di->periodID = gf_strdup(opts+7);
			else if (!strnicmp(opts, "BaseURL=", 8)) {
				di->baseURL = (char **)gf_realloc(di->baseURL, (di->nb_baseURL+1)*sizeof(char *));
				di->baseURL[di->nb_baseURL] = gf_strdup(opts+8);
				di->nb_baseURL++;
			} else if (!strnicmp(opts, "bandwidth=", 10)) di->bandwidth = atoi(opts+10);
			else if (!strnicmp(opts, "role=", 5)) {
				di->roles = gf_realloc(di->roles, sizeof (char *) * (di->nb_roles+1));
				di->roles[di->nb_roles] = gf_strdup(opts+5);
				di->nb_roles++;
			} else if (!strnicmp(opts, "desc", 4)) {
				u32 *nb_descs=NULL;
				char ***descs=NULL;
				u32 opt_offset=0;
				u32 len;
				if (!strnicmp(opts, "desc_p=", 7)) {
					nb_descs = &di->nb_p_descs;
					descs = &di->p_descs;
					opt_offset = 7;
				} else if (!strnicmp(opts, "desc_as=", 8)) {
					nb_descs = &di->nb_as_descs;
					descs = &di->as_descs;
					opt_offset = 8;
				} else if (!strnicmp(opts, "desc_as_c=", 8)) {
					nb_descs = &di->nb_as_c_descs;
					descs = &di->as_c_descs;
					opt_offset = 10;
				} else if (!strnicmp(opts, "desc_rep=", 8)) {
					nb_descs = &di->nb_rep_descs;
					descs = &di->rep_descs;
					opt_offset = 9;
				}
				if (opt_offset) {
					(*nb_descs)++;
					opts += opt_offset;
					len = (u32) strlen(opts);
					(*descs) = (char **)gf_realloc((*descs), (*nb_descs)*sizeof(char *));
					(*descs)[(*nb_descs)-1] = (char *)gf_malloc((len+1)*sizeof(char));
					strncpy((*descs)[(*nb_descs)-1], opts, len);
					(*descs)[(*nb_descs)-1][len] = 0;
				}

			}
			else if (!strnicmp(opts, "xlink=", 6)) di->xlink = gf_strdup(opts+6);
			else if (!strnicmp(opts, "period_duration=", 16)) {
				di->period_duration = (Double) atof(opts+16);
			}	else if (!strnicmp(opts, "duration=", 9)) {
				di->period_duration = (Double) atof(opts+9); /*legacy: use period_duration instead*/
			}

			if (!sep) break;
			sep[0] = ':';
			opts = sep+1;
		}
		first_opt[0] = '\0';
	}
	di->file_name = name;
	if (!di->representationID) {
		char szRep[100];
		sprintf(szRep, "%d", *nb_dash_inputs);
		di->representationID = gf_strdup(szRep);
	}

	return dash_inputs;
}


Bool log_sys_clock = GF_FALSE;
Bool log_utc_time = GF_FALSE;


char outfile[5000];
GF_Err e;
#ifndef GPAC_DISABLE_SCENE_ENCODER
GF_SMEncodeOptions opts;
#endif
SDPLine *sdp_lines = NULL;
Double interleaving_time, split_duration, split_start, dash_duration, dash_subduration;
Bool dash_duration_strict;
MetaAction *metas = NULL;
TrackAction *tracks = NULL;
TSELAction *tsel_acts = NULL;
u64 movie_time, initial_tfdt;
s32 subsegs_per_sidx;
u32 *brand_add = NULL;
u32 *brand_rem = NULL;
GF_DashSwitchingMode bitstream_switching_mode = GF_DASH_BSMODE_DEFAULT;
u32 i, j, stat_level, hint_flags, info_track_id, import_flags, nb_add, nb_cat, crypt, agg_samples, nb_sdp_ex, max_ptime, split_size, nb_meta_act, nb_track_act, rtp_rate, major_brand, nb_alt_brand_add, nb_alt_brand_rem, old_interleave, car_dur, minor_version, conv_type, nb_tsel_acts, program_number, dump_nal, time_shift_depth, initial_moof_sn, dump_std, import_subtitle;
GF_DashDynamicMode dash_mode=GF_DASH_STATIC;
#ifndef GPAC_DISABLE_SCENE_DUMP
GF_SceneDumpFormat dump_mode;
#endif
Double mpd_live_duration = 0;
Bool HintIt, needSave, FullInter, Frag, HintInter, dump_rtp, regular_iod, remove_sys_tracks, remove_hint, force_new, remove_root_od;
Bool print_sdp, print_info, open_edit, dump_cr, force_ocr, encode, do_log, do_flat, dump_srt, dump_ttxt, do_saf, dump_m2ts, dump_cart, verbose, force_cat, align_cat, pack_wgt, single_group, clean_groups, dash_live, no_fragments_defaults, single_traf_per_moof, tfdt_per_traf, dump_nal_crc;
char *inName, *outName, *arg, *mediaSource, *tmpdir, *input_ctx, *output_ctx, *drm_file, *avi2raw, *cprt, *chap_file, *pes_dump, *itunes_tags, *pack_file, *raw_cat, *seg_name, *dash_ctx_file;
u32 track_dump_type, dump_isom, dump_timestamps;
u32 trackID;
Double min_buffer = 1.5;
s32 ast_offset_ms = 0;
u32 dump_chap = 0;
u32 dump_udta_type = 0;
u32 dump_udta_track = 0;
char **mpd_base_urls = NULL;
u32 nb_mpd_base_urls = 0;
u32 dash_scale = 1000;
Bool insert_utc = GF_FALSE;
const char *udp_dest = NULL;

#ifndef GPAC_DISABLE_MPD
Bool do_mpd = GF_FALSE;
#endif
#ifndef GPAC_DISABLE_SCENE_ENCODER
Bool chunk_mode = GF_FALSE;
#endif
#ifndef GPAC_DISABLE_ISOM_HINTING
Bool HintCopy = 0;
u32 MTUSize = 1450;
#endif
#ifndef GPAC_DISABLE_CORE_TOOLS
Bool do_bin_nhml = GF_FALSE;
#endif
GF_ISOFile *file;
Bool frag_real_time = GF_FALSE;
u64 dash_start_date=0;
GF_DASH_ContentLocationMode cp_location_mode = GF_DASH_CPMODE_ADAPTATION_SET;
Double mpd_update_time = GF_FALSE;
Bool stream_rtp = GF_FALSE;
Bool force_test_mode = GF_FALSE;
Bool force_co64 = GF_FALSE;
Bool live_scene = GF_FALSE;
Bool use_mfra = GF_FALSE;
GF_MemTrackerType mem_track = GF_MemTrackerNone;

Bool dump_iod = GF_FALSE;
GF_DASHPSSHMode pssh_mode = 0;
Bool samplegroups_in_traf = GF_FALSE;
Bool daisy_chain_sidx = GF_FALSE;
Bool single_segment = GF_FALSE;
Bool single_file = GF_FALSE;
Bool segment_timeline = GF_FALSE;
u32 segment_marker = GF_FALSE;
GF_DashProfile dash_profile = GF_DASH_PROFILE_UNKNOWN;
const char *dash_profile_extension = NULL;
const char *dash_cues = NULL;
Bool strict_cues = GF_FALSE;
Bool use_url_template = GF_FALSE;
Bool seg_at_rap = GF_FALSE;
Bool frag_at_rap = GF_FALSE;
Bool adjust_split_end = GF_FALSE;
Bool memory_frags = GF_TRUE;
Bool keep_utc = GF_FALSE;
#ifndef GPAC_DISABLE_ATSC
Bool grab_atsc = GF_FALSE;
s32 atsc_max_segs = -1;
u32 atsc_stats_rate = 0;
u32 atsc_debug_tsi = 0;
const char *atsc_output_dir = NULL;
s32 atsc_service = -1;
#endif
u32 timescale = 0;
const char *do_wget = NULL;
GF_DashSegmenterInput *dash_inputs = NULL;
u32 nb_dash_inputs = 0;
char *seg_ext = NULL;
const char *dash_title = NULL;
const char *dash_source = NULL;
const char *dash_more_info = NULL;
#if !defined(GPAC_DISABLE_STREAMING)
const char *grab_m2ts = NULL;
#endif
const char *grab_ifce = NULL;

FILE *logfile = NULL;
static u32 dash_cumulated_time,dash_prev_time,dash_now_time;
//static Bool split_on_bound=GF_FALSE;
//static Bool split_on_closest=GF_FALSE;

u32 mp4box_cleanup(u32 ret_code) {
	if (mpd_base_urls) {
		gf_free(mpd_base_urls);
		mpd_base_urls = NULL;
	}
	if (sdp_lines) {
		gf_free(sdp_lines);
		sdp_lines = NULL;
	}
	if (metas) {
		gf_free(metas);
		metas = NULL;
	}
	if (tracks) {
		for (i = 0; i<nb_track_act; i++) {
			if (tracks[i].out_name)
				gf_free(tracks[i].out_name);
			if (tracks[i].src_name)
				gf_free(tracks[i].src_name);
			if (tracks[i].kind_scheme)
				gf_free(tracks[i].kind_scheme);
			if (tracks[i].kind_value)
				gf_free(tracks[i].kind_value);
		}
		gf_free(tracks);
		tracks = NULL;
	}
	if (tsel_acts) {
		gf_free(tsel_acts);
		tsel_acts = NULL;
	}
	if (brand_add) {
		gf_free(brand_add);
		brand_add = NULL;
	}
	if (brand_rem) {
		gf_free(brand_rem);
		brand_rem = NULL;
	}
	if (dash_inputs) {
		u32 i, j;
		for (i = 0; i<nb_dash_inputs; i++) {
			GF_DashSegmenterInput *di = &dash_inputs[i];
			if (di->nb_baseURL) {
				for (j = 0; j<di->nb_baseURL; j++) {
					gf_free(di->baseURL[j]);
				}
				gf_free(di->baseURL);
			}
			if (di->rep_descs) {
				for (j = 0; j<di->nb_rep_descs; j++) {
					gf_free(di->rep_descs[j]);
				}
				gf_free(di->rep_descs);
			}
			if (di->as_descs) {
				for (j = 0; j<di->nb_as_descs; j++) {
					gf_free(di->as_descs[j]);
				}
				gf_free(di->as_descs);
			}
			if (di->as_c_descs) {
				for (j = 0; j<di->nb_as_c_descs; j++) {
					gf_free(di->as_c_descs[j]);
				}
				gf_free(di->as_c_descs);
			}
			if (di->p_descs) {
				for (j = 0; j<di->nb_p_descs; j++) {
					gf_free(di->p_descs[j]);
				}
				gf_free(di->p_descs);
			}
			if (di->representationID) gf_free(di->representationID);
			if (di->periodID) gf_free(di->periodID);
			if (di->xlink) gf_free(di->xlink);

			if (di->roles) {
				for (j = 0; j<di->nb_roles; j++) {
					gf_free(di->roles[j]);
				}
				gf_free(di->roles);
			}
		}
		gf_free(dash_inputs);
		dash_inputs = NULL;
	}
	if (logfile) gf_fclose(logfile);
	gf_sys_close();
	return ret_code;
}

void init_libgpac(Bool show_logs){
    /*init libgpac*/
    gf_sys_init(mem_track);
    if (show_logs){
	gf_log_set_tool_level(GF_LOG_ALL, GF_LOG_DEBUG);
    }
}

int bs2mp4(char* src, char *output, Double import_fps)
{
	nb_add = 0;
	e = GF_OK;
	import_fps = 0;
	import_flags = 0;
	agg_samples = force_new = GF_FALSE;
	do_flat = GF_FALSE;
	inName = outName = mediaSource = input_ctx = output_ctx = \
	drm_file = avi2raw = cprt = chap_file = pack_file = raw_cat = NULL;
//设置参数
	nb_add = 1; 		//对应 ‘-add’ 个数
	force_new = GF_TRUE; 	//对应'-new'参数
	inName = output;
	
	import_flags = 0;
	agg_samples = 0; //'-agg'参数

	if (!interleaving_time) {
		/*by default use single fragment per dash segment*/
		if (dash_duration)
			interleaving_time = dash_duration;
		else if (!do_flat) {
			interleaving_time = DEFAULT_INTERLEAVING_IN_SEC;//interleaving_time:0.500000
		}
	}
#if !defined(GPAC_DISABLE_MEDIA_IMPORT) && !defined(GPAC_DISABLE_ISOM_WRITE)
	if (nb_add) {
	    u8 open_mode = GF_ISOM_OPEN_EDIT;
     	    if (force_new) {
		open_mode = (do_flat) ? GF_ISOM_OPEN_WRITE : GF_ISOM_WRITE_EDIT;
	    } else {
		FILE *test = gf_fopen(inName, "rb");
		if (!test) {
	  	    open_mode = (do_flat) ? GF_ISOM_OPEN_WRITE : GF_ISOM_WRITE_EDIT;
		    if (!outName) outName = inName;
		} 
		else {
		    gf_fclose(test);
		    if (! gf_isom_probe_file(inName) ) {
			open_mode = (do_flat) ? GF_ISOM_OPEN_WRITE : GF_ISOM_WRITE_EDIT;
			if (!outName) outName = inName;
		    }
		}
	    }
	    open_edit = GF_TRUE;
	    file = gf_isom_open(inName, open_mode, tmpdir);
	    if (!file) {
		fprintf(stderr, "Cannot open destination file %s: %s\n", inName, gf_error_to_string(gf_isom_last_error(NULL)) );
		return mp4box_cleanup(1);
	    }
	    //链接多个-add
	    for(i = 0; i < nb_add; i++)
	    {
	        e = import_file(file, src, import_flags, import_fps, agg_samples);
	        if(e){
	     	    //fprintf(stderr, "Error importing %s: %s\n", argv[i+1], gf_error_to_string(e));
		    gf_isom_delete(file);
		    return mp4box_cleanup(1);
		}
	    }
	    /*unless explicitly asked, remove all systems tracks*/
	    remove_systems_tracks(file); //if (!keep_sys_tracks) 
 	    needSave = GF_TRUE;
	}
#endif /*!GPAC_DISABLE_MEDIA_IMPORT && !GPAC_DISABLE_ISOM_WRITE*/
	strcpy(outfile, outName ? outName : inName);
	{
	    char *szExt = gf_file_ext_start(outfile);
	    if (szExt)
	    {
		/*turn on 3GP saving*/
		if (!stricmp(szExt, ".3gp") || !stricmp(szExt, ".3gpp") || !stricmp(szExt, ".3g2"))
			conv_type = GF_ISOM_CONV_TYPE_3GPP;
		else if (!stricmp(szExt, ".m4a") || !stricmp(szExt, ".m4v"))
			conv_type = GF_ISOM_CONV_TYPE_IPOD;
		else if (!stricmp(szExt, ".psp"))
			conv_type = GF_ISOM_CONV_TYPE_PSP;
		//remove extension from outfile
		*szExt = 0;
	    }
	}


#ifndef GPAC_DISABLE_ISOM_WRITE
	if (!encode) {
		if (!file) {
			fprintf(stderr, "Nothing to do - exiting\n");
			goto exit;
		}
		if (outName) {
			strcpy(outfile, outName);
		} else {
			char *rel_name = strrchr(inName, GF_PATH_SEPARATOR);
			if (!rel_name) rel_name = strrchr(inName, '/');

			strcpy(outfile, "");
			if (tmpdir) {
				strcpy(outfile, tmpdir);
				if (!strchr("\\/", tmpdir[strlen(tmpdir)-1])) strcat(outfile, "/");
			}
			if (!pack_file) strcat(outfile, "out_");
			strcat(outfile, rel_name ? rel_name + 1 : inName);

			if (pack_file) {
				strcpy(outfile, rel_name ? rel_name + 1 : inName);
				rel_name = strrchr(outfile, '.');
				if (rel_name) rel_name[0] = 0;
				strcat(outfile, ".m21");
			}
		}
	} 

	if (movie_time) {
		gf_isom_set_creation_time(file, movie_time);
		for (i=0; i<gf_isom_get_track_count(file); i++) {
			gf_isom_set_track_creation_time(file, i+1, movie_time);
		}
		needSave = GF_TRUE;
	}

	//KK Reached Here!
	e = gf_isom_make_interleave(file, interleaving_time);
	if(!e && old_interleave) 
	    e = gf_isom_set_storage_mode(file, GF_ISOM_STORE_INTERLEAVED);
	if(e) 
	    goto err_exit;

	if (needSave) {
		if (outName) {
			fprintf(stderr, "Saving to %s: ", outfile);
			gf_isom_set_final_name(file, outfile);
		} else if (encode || pack_file) {
			fprintf(stderr, "Saving to %s: ", gf_isom_get_filename(file) );
		} else {
			fprintf(stderr, "Saving %s: ", inName);
		}
		if (HintIt && FullInter) fprintf(stderr, "Hinted file - Full Interleaving\n");
		else if (FullInter) fprintf(stderr, "Full Interleaving\n");
		else if (do_flat || !interleaving_time) fprintf(stderr, "Flat storage\n");
		else fprintf(stderr, "%.3f secs Interleaving%s\n", interleaving_time, old_interleave ? " - no drift control" : "");

		e = gf_isom_close(file);
		file = NULL;

		if (!e && !outName && !encode && !force_new && !pack_file) {
			e = gf_delete_file(inName);
			if (e) {
				fprintf(stderr, "Error removing file %s\n", inName);
			} else {
				e = gf_move_file(outfile, inName);
				if (e) {
					fprintf(stderr, "Error renaming file %s to %s\n", outfile, inName);
				}
			}
		}
	} 
	else {
		gf_isom_delete(file);
	}

	if (e) {
		fprintf(stderr, "Error: %s\n", gf_error_to_string(e));
		goto err_exit;
	}
	goto exit;

#else
	/*close libgpac*/
	gf_isom_delete(file);
	fprintf(stderr, "Error: Read-only version of MP4Box.\n");
	return mp4box_cleanup(1);
#endif

err_exit:
	/*close libgpac*/
	if (file) gf_isom_delete(file);
	fprintf(stderr, "\n\tError: %s\n", gf_error_to_string(e));
	return mp4box_cleanup(1);

exit:
	mp4box_cleanup(0);

#ifdef GPAC_MEMORY_TRACKING
	if (mem_track && (gf_memory_size() || gf_file_handles_count() )) {
		gf_log_set_tool_level(GF_LOG_MEMORY, GF_LOG_INFO);
		gf_memory_print();
		return 2;
	}
#endif
	return 0;
}

int main(int argc, char** argv)
{	
    u32 start, end, dur;
    Bool show_logs = GF_FALSE;
    Double fps = 30.0000;
    char * outname = "videos/output/HR1.mp4";
    char *src = "videos/input/HR1.hvc:split_tiles";//"videos/bs.hevc";  //对应输入码流文件路径
    init_libgpac(show_logs);
start = gf_sys_clock();
    bs2mp4(src, outname, fps);
end = gf_sys_clock();
dur = end-start;
printf("bs2mp4 cost:%u ms\n", dur);
    return 0;
}

#endif /*GPAC_DISABLE_ISOM*/
