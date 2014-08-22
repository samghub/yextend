/*****************************************************************************
 *
 * YWRAPPER: Help for YARA users.
 * Copyright (C) 2014 by Bayshore Networks, Inc. All Rights Reserved.
 *
 * This file is part of ywrapper.
 *
 * ywrapper is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 * 
 * ywrapper is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 * 
 * You should have received a copy of the GNU General Public License
 * along with ywrapper.  If not, see <http://www.gnu.org/licenses/>.
 *
 *****************************************************************************/

#include "bayshore_content_scan.h"
#include "bayshore_yara_wrapper.h"
#include "wrapper.h"
#include "zl.h"
#include <archive.h>
#include <archive_entry.h>

#include <assert.h>
#include <openssl/md5.h>


int iteration_counter = 0;
int archive_failure_counter = 0;

/*
 * type of scan - const data
 * 
 * order matters - DO NOT CHANGE,
 * add at the bottom of array
 */ 
static const char *type_of_scan[] = {
		"Generic", // internal use only
		"Yara Scan"
};


// function declarations
static void scan_content2 (const uint8_t *buf,
		size_t sz,
		const char *rule_file,
		std::list<security_scan_results_t> *ssr_list,
		const char *parent_file_name,
		void (*cb)(void*, std::list<security_scan_results_t> *, const char *),
		int in_type_of_scan
		);


//////////////////////////////////////////////////////////////
// helper functions

char *str2md5(const char *str, int length) 
{
    int n;
    MD5_CTX c;
    unsigned char digest[16];
    char *out = (char*)malloc(33);

    MD5_Init(&c);

    while (length > 0) {
        if (length > 512) {
            MD5_Update(&c, str, 512);
        } else {
            MD5_Update(&c, str, length);
        }
        length -= 512;
        str += 512;
    }

    MD5_Final(digest, &c);

    for (n = 0; n < 16; ++n) {
        snprintf(&(out[n*2]), 16*2, "%02x", (unsigned int)digest[n]);
    }

    return out;
}

std::string remove_file_extension(const std::string &filename) {
    size_t lastdot = filename.find_last_of(".");
    if (lastdot == std::string::npos)
    	return filename;
    return filename.substr(0, lastdot);
}

bool recurs_threshold_passed(int threshold) {
	int r_threshold = 45;
	if (threshold >= r_threshold)
		return true;
	else
		return false;
}

void increment_recur_counter() {
	iteration_counter++;
	//std::cout << "CNT: " << iteration_counter << std::endl;
}

void increment_archive_failure_counter() {
	archive_failure_counter++;
}

double get_failure_percentage() {
	
	double total = iteration_counter - 1;
	
	if (archive_failure_counter > 0)
		return ((double)total/(double)archive_failure_counter) * 100;
	return 0.0;
}
//////////////////////////////////////////////////////////////

/*
 * callback
 */

/*******
yara_cb
********/


void yara_cb (void *cookie, std::list<security_scan_results_t> *ssr_list, const char *child_file_name)
{
	/*
	 * yara call back
	 * 
	 * this should be the only spot that makes calls out
	 * directly to the bayshore yara wrapper 
	 */
	security_scan_parameters_t *ssp_local = (security_scan_parameters_t *)cookie;
	
	char local_api_yara_results[MAX_YARA_RES_BUF];
	size_t local_api_yara_results_len = 0;
	/*
	 * to use this API the buffer passed in to param 4 (local_api_yara_results)
	 * must be at least MAX_YARA_RES_BUF in size. This is defined in
	 * bayshore_yara_wrapper.h
	 * 
	 */
	if (bayshore_yara_wrapper_api(
			(uint8_t*)ssp_local->buffer,
			ssp_local->buffer_length,
			ssp_local->yara_ruleset_filename,
			local_api_yara_results,
			&local_api_yara_results_len) > 0) {
		
		// hit
		if (local_api_yara_results_len > 0) {
			
			security_scan_results_t ssr;
			// populate struct elements
			ssr.file_scan_type = ssp_local->scan_type;
			ssr.file_scan_result = std::string(local_api_yara_results, local_api_yara_results_len);
			
			if (ssp_local->parent_file_name)
				ssr.parent_file_name = std::string(ssp_local->parent_file_name, strlen(ssp_local->parent_file_name));
			
			if (child_file_name)
				ssr.child_file_name = std::string(child_file_name);

			char *output = str2md5((const char *)ssp_local->buffer, ssp_local->buffer_length);
			if (output) {
				memcpy (ssr.file_signature_md5, output, 33);
				free(output);
			}

			ssr_list->push_back(ssr);
		}
	}
}


/*
 * cpp API
 */

/************
scan_content
************/


void scan_content (
		const uint8_t *buf,
		size_t sz,
		const char *rule_file,
		std::list<security_scan_results_t> *ssr_list,
		const char *parent_file_name,
		void (*cb)(void*, std::list<security_scan_results_t> *, const char *),
		int in_type_of_scan
		)
{
	iteration_counter = 0;
	
	int lin_type_of_scan = -1;
	if (in_type_of_scan < sizeof(type_of_scan) / sizeof(type_of_scan[in_type_of_scan]))
		lin_type_of_scan = in_type_of_scan;
	else
		lin_type_of_scan = 0;
	
	scan_content2(buf, sz, rule_file, ssr_list, parent_file_name, cb, lin_type_of_scan);
}



void scan_content2 (
		const uint8_t *buf,
		size_t sz,
		const char *rule_file,
		std::list<security_scan_results_t> *ssr_list,
		const char *parent_file_name,
		void (*cb)(void*, std::list<security_scan_results_t> *, const char *),
		int in_type_of_scan
		)
{
	if (buf) {
		
		/////////////////////////////////////////////////////
		/*
		 * construct the security_scan_parameters_t
		 * struct to pass to the call back funcs
		 * 
		 * right here populate the rule_file_name (yara)
		 * and the parent_file_name
		 * 
		 * later on, based on detected type, populate
		 * the buffer to be analyzed and its respective
		 * size (length)
		 * 
		 */
		security_scan_parameters_t ssp;
		
		if (in_type_of_scan == 1) {
			if (rule_file)
				snprintf (ssp.yara_ruleset_filename, sizeof(ssp.yara_ruleset_filename), "%s", rule_file);
		}
	    snprintf (ssp.parent_file_name, sizeof(ssp.parent_file_name), "%s", parent_file_name);
		/////////////////////////////////////////////////////
				
		int buffer_type = get_content_type (buf, sz);
		//std::cout << buffer_type << std::endl;
		bool is_buf_archive = is_type_archive(buffer_type);
		
		// archive
		if (is_buf_archive) {
			
			if (recurs_threshold_passed(iteration_counter))
				return;
			
			increment_recur_counter();
			/*
			 * intercept gzip archives and handle them
			 * outside of libarchive
			 */
			if (is_type_gzip(buffer_type)) {
				// gunzip the data
				ZlibInflator_t myzl;
				myzl.Ingest ((uint8_t *)buf, sz);
				
				if (myzl.single_result.data && myzl.single_result.used) {
					
					int lf_type = get_content_type (myzl.single_result.data, myzl.single_result.used);
					
					std::string tmpfname = std::string(parent_file_name);
					
					ssp.buffer = myzl.single_result.data;
					ssp.buffer_length = myzl.single_result.used;
					ssp.file_type = lf_type;

	            	/*
	            	 * cases like tar.gz, this would be the gunzipped
	            	 * tarball
	            	 */
	            	if (is_type_archive(lf_type)) {
	            		
	            		scan_content2 (myzl.single_result.data,
	            				myzl.single_result.used,
	            				rule_file, ssr_list,
	            				(remove_file_extension(std::string(parent_file_name))).c_str(),
	            				cb,
	            				in_type_of_scan);
	            		
					} else {
						
						snprintf (ssp.scan_type, sizeof(ssp.scan_type), "%s (%s) inside GZIP Archive file", type_of_scan[in_type_of_scan], get_content_type_string (lf_type));
						
						cb((void *)&ssp, ssr_list, (remove_file_extension(tmpfname)).c_str());

					}
				} // end if (myzl.single_result.data && myzl.single_result.used)
				
			} else {
			
				// prep stuff for libarchive
				struct archive *a = archive_read_new();
				assert(a);
				struct archive_entry *entry;
				int r;
	
				archive_read_support_format_all(a);
				// pre-v4 libarchive
				//archive_read_support_compression_all(a);
				// v4 libarchive
				archive_read_support_filter_all(a);
	
				r = archive_read_open_memory(a, (uint8_t *)buf, sz);
				
				if (r < 0) {
	
				} else {
					/*
					 * libarchive understood the archival tech in
					 * place with the data in buffer file_content ...
					 */
					
					// final sets of data
					uint8_t *final_buff = (uint8_t*) malloc (2048);
					final_buff[0] = 0;
					size_t final_size = 0;
	
					for (;;) {
						r = archive_read_next_header(a, &entry);
						
						if (r == ARCHIVE_EOF)
							break;
						
						if (r != ARCHIVE_OK)
							break;
						
						if (r < ARCHIVE_WARN)
							break;
						
						if (archive_entry_size(entry) > 0) {
							
							char *fname = strdup(archive_entry_pathname(entry));
	
							int x;
							const void *buff;
							size_t lsize;
							off_t offset;
	
							for (;;) {
								
								x = archive_read_data_block(a, &buff, &lsize, &offset);
	
								// hit EOF so process constructed buffer
								if (x == ARCHIVE_EOF) {
									
									if (recurs_threshold_passed(iteration_counter))
										return;
									
									increment_recur_counter();
	
									final_buff[final_size] = 0;
									int lf_type = get_content_type (final_buff, final_size);
									
									ssp.buffer = final_buff;
									ssp.buffer_length = final_size;
									ssp.file_type = lf_type;
	
	
									// archive, make recursive call into scan_content
									if (is_type_archive(lf_type)) {
										
										scan_content2 (final_buff, final_size, rule_file, ssr_list, fname, cb, in_type_of_scan);
										
									} else {
										
										snprintf (ssp.scan_type, sizeof(ssp.scan_type), "%s (%s) inside %s file", type_of_scan[in_type_of_scan], get_content_type_string (lf_type), archive_format_name(a));
										snprintf (ssp.parent_file_name, sizeof(ssp.parent_file_name), "%s", parent_file_name);
										
										cb((void *)&ssp, ssr_list, fname ? fname : "");
						
									}
									
									// reset to 0
									final_size = 0;
									break;
									
								} else if (x == ARCHIVE_OK) {
	
									/*
									 * good to go ... 
									 * 
									 * extend final_buffer, write data to the
									 * end of it, and terminate back inside of
									 * if (x == ARCHIVE_EOF)
									 */
									
									final_size += lsize;
									// extra byte final_size + 1 is for the guard byte
									final_buff = (uint8_t*) realloc (final_buff, final_size + 1);
									assert(final_buff);
									assert(offset + lsize <= final_size);
									memcpy(final_buff + offset, buff, lsize);
									
								} else if (x == ARCHIVE_FAILED) {
									
									increment_recur_counter();
									increment_archive_failure_counter();
									break;
									
								} else {
									
									break;
									
								} // end if else if
							} // end for
							if (fname)
								free(fname);
						} // end if
					} // end for
					if (final_buff)
						free(final_buff);
				} // end if else
	
				// free up libarchive resources
				archive_read_close(a);
				archive_read_free(a);
			} // end if gzip else
		
		} else { // not an archive
			/*
			 * if we are here then we are not dealing
			 * with an archive in the buffer, i.e. not
			 * a zip or gzip or tarball 
			 */
			
			if (recurs_threshold_passed(iteration_counter))
				return;
			
			increment_recur_counter();
			
			ssp.buffer = buf;
			ssp.buffer_length = sz;
			ssp.file_type = buffer_type;
			snprintf (ssp.scan_type, sizeof(ssp.scan_type), "%s (%s)", type_of_scan[in_type_of_scan], get_content_type_string (buffer_type));
				
			cb((void *)&ssp, ssr_list, "");

		} // end if else - archive

	
		/*
		std::cout << "FAILURES: " << archive_failure_counter << std::endl;
		std::cout << "ITERATIONS: " << iteration_counter << std::endl;
		std::cout << "PERC: " << get_failure_percentage() << std::endl;
		*/
		/////////////////////////////////////////////////////
		if (get_failure_percentage() > 90) {
			
			security_scan_results_t ssr;
			// populate struct elements
			ssr.file_scan_type = "Archive Anomaly Scan";
			
			ssr.file_scan_result = "Anomalies present in Archive (possible Decompression Bomb)";
			
			if (parent_file_name)
				ssr.parent_file_name = std::string(parent_file_name);
	
			char *output = str2md5((const char *)buf, sz);
			if (output) {
				memcpy (ssr.file_signature_md5, output, 33);
				free(output);
			}
			ssr_list->push_back(ssr);
		}
		/////////////////////////////////////////////////////
	} // end if (buf)
}





