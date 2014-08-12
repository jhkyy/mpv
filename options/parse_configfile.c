/*
 * This file is part of MPlayer.
 *
 * MPlayer is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * MPlayer is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with MPlayer; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <assert.h>

#include "osdep/io.h"

#include "parse_configfile.h"
#include "common/msg.h"
#include "misc/ctype.h"
#include "m_option.h"
#include "m_config.h"

/// Maximal include depth.
#define MAX_RECURSION_DEPTH 8

// Load options and profiles from from a config file.
//  conffile: path to the config file
//  initial_section: default section where to add normal options
//  flags: M_SETOPT_* bits
//  returns: 1 on sucess, -1 on error, 0 if file not accessible.
int m_config_parse_config_file(m_config_t *config, const char *conffile,
                               char *initial_section, int flags)
{
#define PRINT_LINENUM   MP_ERR(config, "%s:%d: ", conffile, line_num)
#define MAX_LINE_LEN    10000
#define MAX_OPT_LEN     1000
#define MAX_PARAM_LEN   1500
    FILE *fp = NULL;
    char *line = NULL;
    char opt[MAX_OPT_LEN + 1];
    char param[MAX_PARAM_LEN + 1];
    char c;             /* for the "" and '' check */
    int tmp;
    int line_num = 0;
    int line_pos;       /* line pos */
    int opt_pos;        /* opt pos */
    int param_pos;      /* param pos */
    int ret = 1;
    int errors = 0;
    m_profile_t *profile = m_config_add_profile(config, initial_section);

    flags = flags | M_SETOPT_FROM_CONFIG_FILE;

    MP_VERBOSE(config, "Reading config file %s\n", conffile);

    if (config->recursion_depth > MAX_RECURSION_DEPTH) {
        MP_ERR(config, "Maximum 'include' nesting depth exceeded.\n");
        ret = -1;
        goto out;
    }

    if ((line = malloc(MAX_LINE_LEN + 1)) == NULL) {
        ret = -1;
        goto out;
    } else

        MP_VERBOSE(config, "\n");

    if ((fp = fopen(conffile, "r")) == NULL) {
        MP_VERBOSE(config, "Can't open config file: %s\n", strerror(errno));
        ret = 0;
        goto out;
    }

    while (fgets(line, MAX_LINE_LEN, fp)) {
        if (errors >= 16) {
            MP_FATAL(config, "too many errors\n");
            goto out;
        }

        line_num++;
        line_pos = 0;

        /* skip BOM */
        if (strncmp(line, "\xEF\xBB\xBF", 3) == 0)
            line_pos += 3;

        /* skip whitespaces */
        while (mp_isspace(line[line_pos]))
            ++line_pos;

        /* EOL / comment */
        if (line[line_pos] == '\0' || line[line_pos] == '#')
            continue;

        /* read option. */
        for (opt_pos = 0; mp_isprint(line[line_pos]) &&
             line[line_pos] != ' ' &&
             line[line_pos] != '#' &&
             line[line_pos] != '='; /* NOTHING */) {
            opt[opt_pos++] = line[line_pos++];
            if (opt_pos >= MAX_OPT_LEN) {
                PRINT_LINENUM;
                MP_ERR(config, "option name too long\n");
                errors++;
                ret = -1;
                goto nextline;
            }
        }
        if (opt_pos == 0) {
            PRINT_LINENUM;
            MP_ERR(config, "parse error\n");
            ret = -1;
            errors++;
            continue;
        }
        opt[opt_pos] = '\0';

        /* Profile declaration */
        if (opt_pos > 2 && opt[0] == '[' && opt[opt_pos - 1] == ']') {
            opt[opt_pos - 1] = '\0';
            profile = m_config_add_profile(config, opt + 1);
            continue;
        }

        /* skip whitespaces */
        while (mp_isspace(line[line_pos]))
            ++line_pos;

        param_pos = 0;
        bool param_set = false;

        /* check '=' */
        if (line[line_pos] == '=') {
            line_pos++;
            param_set = true;

            /* whitespaces... */
            while (mp_isspace(line[line_pos]))
                ++line_pos;

            /* read the parameter */
            if (line[line_pos] == '"' || line[line_pos] == '\'') {
                c = line[line_pos];
                ++line_pos;
                for (param_pos = 0; line[line_pos] != c; /* NOTHING */) {
                    param[param_pos++] = line[line_pos++];
                    if (param_pos >= MAX_PARAM_LEN) {
                        PRINT_LINENUM;
                        MP_ERR(config, "option %s has a too long parameter\n", opt);
                        ret = -1;
                        errors++;
                        goto nextline;
                    }
                }
                line_pos++;                 /* skip the closing " or ' */
                goto param_done;
            }

            if (line[line_pos] == '%') {
                char *start = &line[line_pos + 1];
                char *end = start;
                unsigned long len = strtoul(start, &end, 10);
                if (start != end && end[0] == '%') {
                    if (len >= MAX_PARAM_LEN - 1 ||
                        strlen(end + 1) < len)
                    {
                        PRINT_LINENUM;
                        MP_ERR(config, "bogus %% length\n");
                        ret = -1;
                        errors++;
                        goto nextline;
                    }
                    param_pos = snprintf(param, sizeof(param), "%.*s",
                                         (int)len, end + 1);
                    line_pos += 1 + (end - start) + 1 + len;
                    goto param_done;
                }
            }

            for (param_pos = 0; mp_isprint(line[line_pos])
                    && !mp_isspace(line[line_pos])
                    && line[line_pos] != '#'; /* NOTHING */) {
                param[param_pos++] = line[line_pos++];
                if (param_pos >= MAX_PARAM_LEN) {
                    PRINT_LINENUM;
                    MP_ERR(config, "too long parameter\n");
                    ret = -1;
                    errors++;
                    goto nextline;
                }
            }

        param_done:

            while (mp_isspace(line[line_pos]))
                ++line_pos;
        }
        param[param_pos] = '\0';

        /* EOL / comment */
        if (line[line_pos] != '\0' && line[line_pos] != '#') {
            PRINT_LINENUM;
            MP_ERR(config, "extra characters: %s\n", line + line_pos);
            ret = -1;
        }

        bstr bopt = bstr0(opt);
        bstr bparam = bstr0(param);

        if (bopt.len >= 3)
            bstr_eatstart0(&bopt, "--");

        if (profile && bstr_equals0(bopt, "profile-desc")) {
            m_profile_set_desc(profile, bparam);
            goto nextline;
        }

        bool need_param = m_config_option_requires_param(config, bopt) > 0;
        if (need_param && !param_set) {
            PRINT_LINENUM;
            MP_ERR(config, "error parsing option %.*s=%.*s: %s\n",
                   BSTR_P(bopt), BSTR_P(bparam),
                   m_option_strerror(M_OPT_MISSING_PARAM));
            continue;
        }

        if (profile) {
            tmp = m_config_set_profile_option(config, profile, bopt, bparam);
        } else {
            tmp = m_config_set_option_ext(config, bopt, bparam, flags);
        }
        if (tmp < 0) {
            PRINT_LINENUM;
            MP_ERR(config, "setting option %.*s='%.*s' failed.\n",
                   BSTR_P(bopt), BSTR_P(bparam));
            continue;
            /* break */
        }
nextline:
        ;
    }

out:
    free(line);
    if (fp)
        fclose(fp);
    config->recursion_depth -= 1;
    if (ret < 0) {
        MP_FATAL(config, "Error loading config file %s.\n",
               conffile);
    }
    return ret;
}
