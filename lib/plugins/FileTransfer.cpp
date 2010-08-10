/*
    FileTransfer.cpp

    Copyright (C) 2009  Daniel Novotny (dnovotny@redhat.com)
    Copyright (C) 2009  RedHat inc.

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
#ifndef _GNU_SOURCE
# define _GNU_SOURCE
#endif
#include <libtar.h>
#include <bzlib.h>
#include <zlib.h>
#include "abrtlib.h"
#include "abrt_curl.h"
#include "FileTransfer.h"
#include "debug_dump.h"
#include "abrt_exception.h"
#include "comm_layer_inner.h"

using namespace std;

#define HBLEN 255
#define FILETRANSFER_DIRLIST DEBUG_DUMPS_DIR "/FileTransferDirlist.txt"

CFileTransfer::CFileTransfer()
:
    m_sArchiveType(".tar.gz"),
    m_nRetryCount(3),
    m_nRetryDelay(20)
{
}

void CFileTransfer::SendFile(const char *pURL, const char *pFilename)
{
    int len = strlen(pURL);
    if (len == 0)
    {
        error_msg(_("FileTransfer: URL not specified"));
        return;
    }

    update_client(_("Sending archive %s to %s"), pFilename, pURL);

    string wholeURL = concat_path_file(pURL, strrchr(pFilename, '/') ? : pFilename);

    int count = m_nRetryCount;
    while (1)
    {
        FILE *f = fopen(pFilename, "r");
        if (!f)
        {
            throw CABRTException(EXCEP_PLUGIN, "Can't open archive file '%s'", pFilename);
        }

        struct stat buf;
        fstat(fileno(f), &buf); /* never fails */

        CURL *curl = xcurl_easy_init();
        /* enable uploading */
        curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L);
        /* specify target */
        curl_easy_setopt(curl, CURLOPT_URL, wholeURL.c_str());
        /* FILE handle: passed to the default callback, it will fread() it */
        curl_easy_setopt(curl, CURLOPT_READDATA, f);
        curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, (curl_off_t)buf.st_size);

        /* everything is done here; result 0 means success */
        int result = curl_easy_perform(curl);

        curl_easy_cleanup(curl);
        fclose(f);
        if (result == 0 || --count <= 0)
            break;
        /* retry the upload if not succesful, wait a bit before next try */
        sleep(m_nRetryDelay);
    }
}

static void create_tar(const char *archive_name, const char *directory)
{
    TAR *tar;

    if (tar_open(&tar, (char *)archive_name, NULL, O_WRONLY | O_CREAT, 0644, TAR_GNU) != 0)
    {
        return;
    }
    tar_append_tree(tar, (char *)directory, (char*)".");
    tar_close(tar);
}

static void create_targz(const char *archive_name, const char *directory)
{
    char *name_without_gz = xstrdup(archive_name);
    strrchr(name_without_gz, '.')[0] = '\0';

    create_tar(name_without_gz, directory);

    int fd = open(name_without_gz, O_RDONLY);
    if (fd < 0)
    {
        remove(name_without_gz);
        free(name_without_gz);
        return;
    }

    gzFile gz = gzopen(archive_name, "w");
    if (gz == NULL)
    {
        close(fd);
        remove(name_without_gz);
        free(name_without_gz);
        return;
    }

    char buf[BUFSIZ];
    ssize_t bytesRead;
    while ((bytesRead = full_read(fd, buf, BUFSIZ)) > 0)
    {
        gzwrite(gz, buf, bytesRead); // TODO: check that return value == bytesRead
    }

    gzclose(gz);
    close(fd);
    remove(name_without_gz);
    free(name_without_gz);
}

static void create_tarbz2(const char * archive_name, const char * directory)
{
    char *name_without_bz2 = xstrdup(archive_name);
    strrchr(name_without_bz2, '.')[0] = '\0';

    create_tar(name_without_bz2, directory);

    int tarFD = open(name_without_bz2, O_RDONLY);
    if (tarFD == -1)
    {
        remove(name_without_bz2);
        free(name_without_bz2);
        return;
    }
    FILE *f = fopen(archive_name, "w");
    if (f == NULL)
    {
        close(tarFD);
        remove(name_without_bz2);
        free(name_without_bz2);
        return;
    }
    int bzError;
    BZFILE *bz = BZ2_bzWriteOpen(&bzError, f, /*BLOCK_MULTIPLIER:*/ 7, 0, 0);
    if (bz == NULL)
    {
        fclose(f);
        close(tarFD);
        remove(name_without_bz2);
        free(name_without_bz2);
        return;
    }

    char buf[BUFSIZ];
    ssize_t bytesRead;
    while ((bytesRead = read(tarFD, buf, BUFSIZ)) > 0)
    {
        BZ2_bzWrite(&bzError, bz, buf, bytesRead);
    }

    BZ2_bzWriteClose(&bzError, bz, 0, NULL, NULL);
    fclose(f);
    close(tarFD);
    remove(name_without_bz2);
    free(name_without_bz2);
}

void CFileTransfer::CreateArchive(const char *pArchiveName, const char *pDir)
{
    if (m_sArchiveType == ".tar")
    {
        create_tar(pArchiveName, pDir);
    }
    else if (m_sArchiveType == ".tar.gz")
    {
        create_targz(pArchiveName, pDir);
    }
    else if (m_sArchiveType == ".tar.bz2")
    {
        create_tarbz2(pArchiveName, pDir);
    }
    else
    {
        throw CABRTException(EXCEP_PLUGIN, "Unknown/unsupported archive type %s", m_sArchiveType.c_str());
    }
}

/* Returns the last component of the directory path.
 * Careful to not return "" on "/path/path2/", but "path2".
 */
static string DirBase(const char *pStr)
{
    int end = strlen(pStr);
    if (end > 1 && pStr[end-1] == '/')
    {
        end--;
    }
    int beg = end;
    while (beg > 0 && pStr[beg-1] != '/')
    {
        beg--;
    }
    return string(pStr + beg, end - beg);
}

void CFileTransfer::Run(const char *pActionDir, const char *pArgs, int force)
{
    if (strcmp(pArgs, "store") == 0)
    {
        /* Remember pActiveDir for later sending */
        FILE *dirlist = fopen(FILETRANSFER_DIRLIST, "a");
        fprintf(dirlist, "%s\n", pActionDir);
        fclose(dirlist);
        VERB3 log("Remembered '%s' for future file transfer", pActionDir);
        return;
    }

    update_client(_("FileTransfer: Creating a report..."));

    char hostname[HBLEN];
    gethostname(hostname, HBLEN-1);
    hostname[HBLEN-1] = '\0';

    char tmpdir_name[] = "/tmp/abrtuploadXXXXXX";
    /* mkdtemp does mkdir(xxx, 0700), should be safe (is it?) */
    if (mkdtemp(tmpdir_name) == NULL)
    {
        throw CABRTException(EXCEP_PLUGIN, "Can't mkdir a temporary directory in /tmp");
    }

    if (strcmp(pArgs, "one") == 0)
    {
        /* Just send one archive */
        string archivename = ssprintf("%s/%s-%s%s", tmpdir_name, hostname, DirBase(pActionDir).c_str(), m_sArchiveType.c_str());
        try
        {
            CreateArchive(archivename.c_str(), pActionDir);
            SendFile(m_sURL.c_str(), archivename.c_str());
        }
        catch (CABRTException& e)
        {
            error_msg(_("Cannot create and send an archive: %s"), e.what());
        }
        unlink(archivename.c_str());
    }
    else
    {
        /* Tar up and send all remebered directories */
        FILE *dirlist = fopen(FILETRANSFER_DIRLIST, "r");
        if (!dirlist)
        {
            /* not an error */
            VERB3 log("No saved crashes to transfer");
            goto del_tmp_dir;
        }

        char dirname[PATH_MAX];
        while (fgets(dirname, sizeof(dirname), dirlist) != NULL)
        {
            strchrnul(dirname, '\n')[0] = '\0';
            string archivename = ssprintf("%s/%s-%s%s", tmpdir_name, hostname, DirBase(dirname).c_str(), m_sArchiveType.c_str());
            try
            {
        	VERB3 log("Creating archive '%s' of dir '%s'", archivename.c_str(), dirname);
                CreateArchive(archivename.c_str(), dirname);
                VERB3 log("Sending archive to '%s'", m_sURL.c_str());
                SendFile(m_sURL.c_str(), archivename.c_str());
            }
            catch (CABRTException& e)
            {
                error_msg(_("Cannot create and send an archive: %s"), e.what());
            }
            VERB3 log("Deleting archive '%s'", archivename.c_str());
            unlink(archivename.c_str());
        }

        fclose(dirlist);
        /* all the files we're able to send should be sent now,
           starting over with clean table */
        unlink(FILETRANSFER_DIRLIST);
    }

 del_tmp_dir:
    rmdir(tmpdir_name);
}

void CFileTransfer::SetSettings(const map_plugin_settings_t& pSettings)
{
    m_pSettings = pSettings;

    map_plugin_settings_t::const_iterator end = pSettings.end();
    map_plugin_settings_t::const_iterator it;
    it = pSettings.find("URL");
    if (it != end)
    {
        m_sURL = it->second;
    }

    it = pSettings.find("RetryCount");
    if (it != end)
    {
        m_nRetryCount = xatoi_u(it->second.c_str());
    }

    it = pSettings.find("RetryDelay");
    if (it != end)
    {
        m_nRetryDelay = xatoi_u(it->second.c_str());
    }

    it = pSettings.find("ArchiveType");
    if (it != end)
    {
        /* currently supporting .tar, .tar.gz, .tar.bz2 and .zip */
        m_sArchiveType = it->second;
        if (m_sArchiveType[0] != '.')
        {
            m_sArchiveType = "." + m_sArchiveType;
        }
    }
}

//ok to delete?
//const map_plugin_settings_t& CFileTransfer::GetSettings()
//{
//    m_pSettings["URL"] = m_sURL;
//    m_pSettings["RetryCount"] = to_string(m_nRetryCount);
//    m_pSettings["RetryDelay"] = to_string(m_nRetryDelay);
//    m_pSettings["ArchiveType"] = m_sArchiveType;
//
//    return m_pSettings;
//}

PLUGIN_INFO(ACTION,
            CFileTransfer,
            "FileTransfer",
            "0.0.6",
            _("Sends a report via FTP or SCTP"),
            "dnovotny@redhat.com",
            "https://fedorahosted.org/abrt/wiki",
            "");