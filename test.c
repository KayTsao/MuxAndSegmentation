
#include <gpac/download.h>
#include <gpac/network.h>
#include <gpac/utf.h>

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

#ifndef GPAC_DISABLE_MEDIA_IMPORT
GF_Err import_file(GF_ISOFile *dest, char *inName, u32 import_flags, Double force_fps, u32 frames_per_sample);
#else
GF_Err import_file(GF_ISOFile *dest, char *inName, u32 import_flags, Double force_fps, u32 frames_per_sample) {
	return GF_NOT_SUPPORTED;
}
#endif
GF_Err split_isomedia_file(GF_ISOFile *mp4, Double split_dur, u64 split_size_kb, char *inName, Double interleaving_time, Double chunk_start, Bool adjust_split_end, char *outName, const char *tmpdir);
GF_Err cat_isomedia_file(GF_ISOFile *mp4, char *fileName, u32 import_flags, Double force_fps, u32 frames_per_sample, char *tmp_dir, Bool force_cat, Bool align_timelines, Bool allow_add_in_command);
#if !defined(GPAC_DISABLE_SCENE_ENCODER)
//GF_Err EncodeFile(char *in, GF_ISOFile *mp4, GF_SMEncodeOptions *opts, FILE *logs);
//GF_Err EncodeFileChunk(char *chunkFile, char *bifs, char *inputContext, char *outputContext, const char *tmpdir);
#endif
GF_ISOFile *package_file(char *file_name, char *fcc, const char *tmpdir, Bool make_wgt);
#endif

GF_Err dump_isom_cover_art(GF_ISOFile *file, char *inName, Bool is_final_name);
GF_Err dump_isom_chapters(GF_ISOFile *file, char *inName, Bool is_final_name, Bool dump_ogg);
void dump_isom_udta(GF_ISOFile *file, char *inName, Bool is_final_name, u32 dump_udta_type, u32 dump_udta_track);
GF_Err set_file_udta(GF_ISOFile *dest, u32 tracknum, u32 udta_type, char *src, Bool is_box_array);
u32 id3_get_genre_tag(const char *name);

/*in filedump.c*/
#ifndef GPAC_DISABLE_SCENE_DUMP
//GF_Err dump_isom_scene(char *file, char *inName, Bool is_final_name, GF_SceneDumpFormat dump_mode, Bool do_log);
//void gf_check_isom_files(char *conf_rules, char *inName);
#endif
#ifndef GPAC_DISABLE_SCENE_STATS
void dump_isom_scene_stats(char *file, char *inName, Bool is_final_name, u32 stat_level);
#endif
void PrintNode(const char *name, u32 graph_type);
void PrintBuiltInNodes(u32 graph_type);
void PrintBuiltInBoxes();

#ifndef GPAC_DISABLE_ISOM_DUMP
GF_Err dump_isom_xml(GF_ISOFile *file, char *inName, Bool is_final_name, Bool do_track_dump);
#endif


#ifndef GPAC_DISABLE_ISOM_HINTING
#ifndef GPAC_DISABLE_ISOM_DUMP
void dump_isom_rtp(GF_ISOFile *file, char *inName, Bool is_final_name);
#endif
void dump_isom_sdp(GF_ISOFile *file, char *inName, Bool is_final_name);
#endif

void dump_isom_timestamps(GF_ISOFile *file, char *inName, Bool is_final_name, Bool skip_offset);
void dump_isom_nal(GF_ISOFile *file, u32 trackID, char *inName, Bool is_final_name, Bool dump_crc);

#ifndef GPAC_DISABLE_ISOM_DUMP
void dump_isom_ismacryp(GF_ISOFile *file, char *inName, Bool is_final_name);
void dump_isom_timed_text(GF_ISOFile *file, u32 trackID, char *inName, Bool is_final_name, Bool is_convert, GF_TextDumpType dump_type);
#endif /*GPAC_DISABLE_ISOM_DUMP*/



void DumpTrackInfo(GF_ISOFile *file, u32 trackID, Bool full_dump);
void DumpMovieInfo(GF_ISOFile *file);
void PrintLanguages();

#ifndef GPAC_DISABLE_MPEG2TS
void dump_mpeg2_ts(char *mpeg2ts_file, char *pes_out_name, Bool prog_num);
#endif


#if !defined(GPAC_DISABLE_STREAMING) && !defined(GPAC_DISABLE_SENG)
void PrintStreamerUsage();
int stream_file_rtp(int argc, char **argv);
int live_session(int argc, char **argv);
void PrintLiveUsage();
#endif

#if !defined(GPAC_DISABLE_STREAMING)
u32 grab_live_m2ts(const char *grab_m2ts, const char *ifce_name, const char *outName);
#endif

#ifndef GPAC_DISABLE_ATSC
u32 grab_atsc3_session(const char *dir, const char *ifce, s32 serviceID, s32 max_segs, u32 stats_rate, u32 debug_tsi);
#endif



GF_Err dump_isom_cover_art(GF_ISOFile *file, char *inName, Bool is_final_name);
GF_Err dump_isom_chapters(GF_ISOFile *file, char *inName, Bool is_final_name, Bool dump_ogg);
void dump_isom_udta(GF_ISOFile *file, char *inName, Bool is_final_name, u32 dump_udta_type, u32 dump_udta_track);
GF_Err set_file_udta(GF_ISOFile *dest, u32 tracknum, u32 udta_type, char *src, Bool is_box_array);
u32 id3_get_genre_tag(const char *name);



Bool keep_sys_tracks = GF_FALSE;
char outfile[5000];
GF_Err e;
GF_ISOFile *file;
Bool force_new, open_edit,do_flat;
u32 i, j,nb_add, import_flags,import_fps,agg_samples;
//u32 i, j, stat_level, hint_flags, info_track_id, import_flags, nb_add, nb_cat, crypt, agg_samples, nb_sdp_ex, max_ptime, split_size, nb_meta_act, nb_track_act, rtp_rate, major_brand, nb_alt_brand_add, nb_alt_brand_rem, old_interleave, car_dur, minor_version, conv_type, nb_tsel_acts, program_number, dump_nal, time_shift_depth, initial_moof_sn, dump_std, import_subtitle, dump_saps, dump_saps_mode;
//GF_DashDynamicMode dash_mode=GF_DASH_STATIC;
//Bool HintIt, needSave, FullInter, Frag, HintInter, dump_rtp, regular_iod, remove_sys_tracks, remove_hint, force_new, remove_root_od;
//Bool print_sdp, print_info, open_edit, dump_cr, force_ocr, encode, do_log, do_flat, dump_srt, dump_ttxt, do_saf, dump_m2ts, dump_cart, do_hash, verbose, force_cat, align_cat, pack_wgt, single_group, clean_groups, dash_live, no_fragments_defaults, single_traf_per_moof, tfdt_per_traf, dump_nal_crc;
char *inName, *outName, *arg, *mediaSource, *tmpdir, *input_ctx, *output_ctx, *drm_file, *avi2raw, *cprt, *chap_file, *pes_dump, *itunes_tags, *pack_file, *raw_cat, *seg_name, *dash_ctx_file;
u32 track_dump_type, dump_isom, dump_timestamps;

int main(int argc, char** argv)
{
    e = GF_OK;    
    nb_add = 1;
    force_new = GF_TRUE;
    do_flat = GF_FALSE;
    file = NULL;
    tmpdir = inName = outName = NULL;
    import_flags = import_fps = agg_samples = 0;
//= mediaSource = input_ctx = output_ctx = drm_file = avi2raw = cprt = chap_file = pack_file = raw_cat = NULL;
    //mem_track = GF_MemTrackerSimple;
    gf_sys_init(GF_MemTrackerSimple);
    gf_log_set_tool_level(GF_LOG_ALL, GF_LOG_DEBUG);
    //gf_log_set_tools_levels("all@debug"); <--THIS FUNCTION LEAD TO SEGMENT FAULT FOR SOME REASON UNKNOWN
    inName = argv[2];
    if(nb_add){
        u8 open_mode = GF_ISOM_OPEN_EDIT;
        if(force_new)
	    open_mode = (do_flat) ? GF_ISOM_OPEN_WRITE : GF_ISOM_WRITE_EDIT;
	else{
	//暂不处理
	}
	open_edit = GF_TRUE;
	file = gf_isom_open(inName, open_mode, tmpdir);
        if(!file){
	  printf("Can't open destination file:%s:%s\n", inName, gf_error_to_string(gf_isom_last_error(NULL)));
    	  return -1; //cleanup()
        }
	//-add parameter leads to:
        char *src = "/media/kaytsao/000A982F000D3151/caomeng/Work_Note/18_Q4/testFFMPEG/Norway5s.hevc";
	e = import_file(file, src, import_flags, import_fps, agg_samples);
    }

/*
    fin = gf_fopen(argv[1], "rb");
    if (!fin) 
	printf("open input error\n");
    fout = gf_fopen(argv[2], "a+b");
    if (!fout) {
	printf("open output error\n");        
    } 

    gf_fclose(fin);
    gf_fclose(fout);
*/
    gf_sys_close();
}
