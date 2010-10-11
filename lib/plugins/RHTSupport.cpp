/*
    Copyright (C) 2010  ABRT team
    Copyright (C) 2010  RedHat Inc

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#define _GNU_SOURCE 1    /* for stpcpy */
#include <libtar.h>
#include "abrtlib.h"
#include "abrt_curl.h"
#include "abrt_rh_support.h"
#include "crash_types.h"
#include "abrt_exception.h"
#include "comm_layer_inner.h"
#include "RHTSupport.h"

using namespace std;


#if 0 //unused
static char *xml_escape(const char *str)
{
    const char *s = str;
    unsigned count = 1; /* for NUL */
    while (*s)
    {
        if (*s == '&')
            count += sizeof("&amp;")-2;
        if (*s == '<')
            count += sizeof("&lt;")-2;
        if (*s == '>')
            count += sizeof("&gt;")-2;
        if ((unsigned char)*s > 126 || (unsigned char)*s < ' ')
            count += sizeof("\\x00")-2;
        count++;
        s++;
    }
    char *result = (char*)xmalloc(count);
    char *d = result;
    s = str;
    while (*s)
    {
        if (*s == '&')
            d = stpcpy(d, "&amp;");
        else if (*s == '<')
            d = stpcpy(d, "&lt;");
        else if (*s == '>')
            d = stpcpy(d, "&gt;");
        else
        if ((unsigned char)*s > 126
         || (  (unsigned char)*s < ' '
            && *s != '\t'
            && *s != '\n'
            && *s != '\r'
            )
        ) {
            *d++ = '\\';
            *d++ = 'x';
            *d++ = "0123456789abcdef"[(unsigned char)*s >> 4];
            *d++ = "0123456789abcdef"[(unsigned char)*s & 0xf];
        }
        else
            *d++ = *s;
        s++;
    }
    *d = '\0';
    return result;
}
#endif


/*
 * CReporterRHticket
 */

CReporterRHticket::CReporterRHticket()
{
    m_login = NULL;
    m_password = NULL;
    m_ssl_verify = true;
    m_strata_url = xstrdup("https://api.access.redhat.com/rs");
}

CReporterRHticket::~CReporterRHticket()
{
    free(m_login);
    free(m_password);
    free(m_strata_url);
}

string CReporterRHticket::Report(const map_crash_data_t& pCrashData,
        const map_plugin_settings_t& pSettings,
        const char *pArgs)
{
    /* Gzipping e.g. 0.5gig coredump takes a while. Let client know what we are doing */
    update_client(_("Compressing data"));

    string retval;

    map_plugin_settings_t::const_iterator end = pSettings.end();
    map_plugin_settings_t::const_iterator it;
    it = pSettings.find("URL");
    char *url = (it == end ? m_strata_url : xstrdup(it->second.c_str()));

    it = pSettings.find("Login");
    char *login = (it == end ? m_login : xstrdup(it->second.c_str()));

    it = pSettings.find("Password");
    char *password = (it == end ? m_password : xstrdup(it->second.c_str()));

    it = pSettings.find("SSLVerify");
    bool ssl_verify = (it == end ? m_ssl_verify : string_to_bool(it->second.c_str()));

    const char *package   = get_crash_data_item_content_or_NULL(pCrashData, FILENAME_PACKAGE);
//  const string& component = get_crash_data_item_content(pCrashData, FILENAME_COMPONENT);
//  const string& release   = get_crash_data_item_content(pCrashData, FILENAME_RELEASE);
//  const string& arch      = get_crash_data_item_content(pCrashData, FILENAME_ARCHITECTURE);
//  const string& duphash   = get_crash_data_item_content(pCrashData, FILENAME_DUPHASH);
    const char *reason      = get_crash_data_item_content_or_NULL(pCrashData, FILENAME_REASON);
    const char *function    = get_crash_data_item_content_or_NULL(pCrashData, FILENAME_CRASH_FUNCTION);

    struct strbuf *buf_summary = strbuf_new();
    strbuf_append_strf(buf_summary, "[abrt] %s", package);

    if (function && strlen(function) < 30)
        strbuf_append_strf(buf_summary, ": %s", function);

    if (reason)
        strbuf_append_strf(buf_summary, ": %s", reason);

    char *summary = strbuf_free_nobuf(buf_summary);

    char *bz_dsc = make_description_bz(pCrashData);
    char *dsc = xasprintf("abrt version: "VERSION"\n%s", bz_dsc);
    free(bz_dsc);

    reportfile_t* file = new_reportfile();

    /* SELinux guys are not happy with /tmp, using /var/run/abrt */
    char *tempfile = xasprintf(LOCALSTATEDIR"/run/abrt/tmp-%lu-%lu.tar.gz", (long)getpid(), (long)time(NULL));

    int pipe_from_parent_to_child[2];
    xpipe(pipe_from_parent_to_child);
    pid_t child = fork();
    if (child == 0)
    {
        /* child */
        close(pipe_from_parent_to_child[1]);
        xmove_fd(xopen3(tempfile, O_WRONLY | O_CREAT | O_EXCL, 0600), 1);
        xmove_fd(pipe_from_parent_to_child[0], 0);
        execlp("gzip", "gzip", NULL);
        perror_msg_and_die("can't execute '%s'", "gzip");
    }
    close(pipe_from_parent_to_child[0]);

    TAR *tar = NULL;
    if (tar_fdopen(&tar, pipe_from_parent_to_child[1], tempfile,
                /*fileops:(standard)*/ NULL, O_WRONLY | O_CREAT, 0644, TAR_GNU) != 0)
    {
        retval = "can't create temporary file in "LOCALSTATEDIR"/run/abrt";
        goto ret;
    }

    {
        map_crash_data_t::const_iterator it = pCrashData.begin();
        for (; it != pCrashData.end(); it++)
        {
            if (it->first == CD_COUNT) continue;
            if (it->first == CD_DUMPDIR) continue;
            if (it->first == CD_INFORMALL) continue;
            if (it->first == CD_REPORTED) continue;
            if (it->first == CD_MESSAGE) continue; // plugin's status message (if we already reported it yesterday)
            if (it->first == FILENAME_DESCRIPTION) continue; // package description

            const char *content = it->second[CD_CONTENT].c_str();
            if (it->second[CD_TYPE] == CD_TXT)
            {
                reportfile_add_binding_from_string(file, it->first.c_str(), content);
            }
            else if (it->second[CD_TYPE] == CD_BIN)
            {
                const char *basename = strrchr(content, '/');
                if (basename)
                    basename++;
                else
                    basename = content;
                char *xml_name = concat_path_file("content", basename);
                reportfile_add_binding_from_namedfile(file,
                        /*on_disk_filename */ content,
                        /*binding_name     */ it->first.c_str(),
                        /*recorded_filename*/ xml_name,
                        /*binary           */ 1);
                if (tar_append_file(tar, (char*)content, xml_name) != 0)
                {
                    retval = "can't create temporary file in "LOCALSTATEDIR"/run/abrt";
                    free(xml_name);
                    goto ret;
                }
                free(xml_name);
            }
        }
    }

    /* Write out content.xml in the tarball's root */
    {
        const char *signature = reportfile_as_string(file);
        unsigned len = strlen(signature);
        unsigned len512 = (len + 511) & ~511;
        char *block = (char*)memcpy(xzalloc(len512), signature, len);
        th_set_type(tar, S_IFREG | 0644);
        th_set_mode(tar, S_IFREG | 0644);
      //th_set_link(tar, char *linkname);
      //th_set_device(tar, dev_t device);
      //th_set_user(tar, uid_t uid);
      //th_set_group(tar, gid_t gid);
      //th_set_mtime(tar, time_t fmtime);
        th_set_path(tar, (char*)"content.xml");
        th_set_size(tar, len);
        th_finish(tar); /* caclulate and store th xsum etc */
        if (th_write(tar) != 0
         || full_write(tar_fd(tar), block, len512) != len512
         || tar_close(tar) != 0
        ) {
            free(block);
            retval = "can't create temporary file in "LOCALSTATEDIR"/run/abrt";
            goto ret;
        }
        tar = NULL;
        free(block);
    }

    {
        update_client(_("Creating a new case..."));
        char* result = send_report_to_new_case(url,
                login,
                password,
                ssl_verify,
                summary,
                dsc,
                package,
                tempfile
        );
        VERB3 log("post result:'%s'", result);
        retval = result;
        free(result);
    }

 ret:
    // Damn, selinux does not allow SIGKILLing our own child! wtf??
    //kill(child, SIGKILL); /* just in case */
    waitpid(child, NULL, 0);
    if (tar)
        tar_close(tar);
    //close(pipe_from_parent_to_child[1]); - tar_close() does it itself
    unlink(tempfile);
    free(tempfile);
    reportfile_free(file);

    free(summary);
    free(dsc);

    if (strncasecmp(retval.c_str(), "error", 5) == 0)
    {
        throw CABRTException(EXCEP_PLUGIN, "%s", retval.c_str());
    }
    return retval;
}

void CReporterRHticket::SetSettings(const map_plugin_settings_t& pSettings)
{
    m_pSettings = pSettings;

    map_plugin_settings_t::const_iterator end = pSettings.end();
    map_plugin_settings_t::const_iterator it;
    it = pSettings.find("URL");
    if (it != end)
    {
        free(m_strata_url);
        m_strata_url = xstrdup(it->second.c_str());
    }
    it = pSettings.find("Login");
    if (it != end)
    {
        free(m_login);
        m_login = xstrdup(it->second.c_str());
    }
    it = pSettings.find("Password");
    if (it != end)
    {
        free(m_password);
        m_password = xstrdup(it->second.c_str());
    }
    it = pSettings.find("SSLVerify");
    if (it != end)
    {
        m_ssl_verify = string_to_bool(it->second.c_str());
    }
}

/* Should not be deleted (why?) */
const map_plugin_settings_t& CReporterRHticket::GetSettings()
{
    m_pSettings["URL"] = (m_strata_url)? m_strata_url: "";
    m_pSettings["Login"] = (m_login)? m_login: "";
    m_pSettings["Password"] = (m_password)? m_password: "";
    m_pSettings["SSLVerify"] = m_ssl_verify ? "yes" : "no";

    return m_pSettings;
}

PLUGIN_INFO(REPORTER,
    CReporterRHticket,
    "RHticket",
    "0.0.4",
    _("Reports bugs to Red Hat support"),
    "Denys Vlasenko <dvlasenk@redhat.com>",
    "https://fedorahosted.org/abrt/wiki",
    PLUGINS_LIB_DIR"/RHTSupport.glade");
