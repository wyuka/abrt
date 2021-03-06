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
#include "abrtlib.h"
#include "abrt_problem_data.h"
#include "parse_options.h"
#include "abrt_xmlrpc.h"
#include "rhbz.h"

#define XML_RPC_SUFFIX      "/xmlrpc.cgi"

/* From RHEL6 kernel/panic.c:
 * { TAINT_PROPRIETARY_MODULE,     'P', 'G' },
 * { TAINT_FORCED_MODULE,          'F', ' ' },
 * { TAINT_UNSAFE_SMP,             'S', ' ' },
 * { TAINT_FORCED_RMMOD,           'R', ' ' },
 * { TAINT_MACHINE_CHECK,          'M', ' ' },
 * { TAINT_BAD_PAGE,               'B', ' ' },
 * { TAINT_USER,                   'U', ' ' },
 * { TAINT_DIE,                    'D', ' ' },
 * { TAINT_OVERRIDDEN_ACPI_TABLE,  'A', ' ' },
 * { TAINT_WARN,                   'W', ' ' },
 * { TAINT_CRAP,                   'C', ' ' },
 * { TAINT_FIRMWARE_WORKAROUND,    'I', ' ' },
 * entries 12 - 27 are unused
 * { TAINT_HARDWARE_UNSUPPORTED,   'H', ' ' },
 * entries 29 - 31 are unused
 */

static const char * const taint_warnings[] = {
    "Proprietary Module",
    "Forced Module",
    "Unsafe SMP",
    "Forced rmmod",
    "Machine Check",
    "Bad Page",
    "User",
    "Die",
    "Overriden ACPI Table",
    "Warning Issued",
    "Experimental Module Loaded",
    "Firmware Workaround",
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    "Hardware Unsupported",
    NULL,
    NULL,
};

/* TODO: npajkovs: fix tainted string */
static const char *tainted_string(unsigned tainted)
{
    unsigned idx = 0;
    while ((tainted >>= 1) != 0)
        idx++;

    return taint_warnings[idx];
}

static void report_to_bugzilla(const char *dump_dir_name, map_string_h *settings)
{
    struct dump_dir *dd = dd_opendir(dump_dir_name, /*flags:*/ 0);
    if (!dd)
        xfunc_die(); /* dd_opendir already emitted error msg */
    problem_data_t *problem_data = create_problem_data_from_dump_dir(dd);
    dd_close(dd);

    const char *env;
    const char *login;
    const char *password;
    const char *bugzilla_xmlrpc;
    const char *bugzilla_url;
    bool ssl_verify;

    env = getenv("Bugzilla_Login");
    login = env ? env : get_map_string_item_or_empty(settings, "Login");
    env = getenv("Bugzilla_Password");
    password = env ? env : get_map_string_item_or_empty(settings, "Password");
    if (!login[0] || !password[0])
        error_msg_and_die(_("Empty login or password, please check your configuration"));

    env = getenv("Bugzilla_BugzillaURL");
    bugzilla_url = env ? env : get_map_string_item_or_empty(settings, "BugzillaURL");
    if (!bugzilla_url[0])
        bugzilla_url = "https://bugzilla.redhat.com";
    bugzilla_xmlrpc = xasprintf("%s"XML_RPC_SUFFIX, bugzilla_url);

    env = getenv("Bugzilla_SSLVerify");
    ssl_verify = string_to_bool(env ? env : get_map_string_item_or_empty(settings, "SSLVerify"));

    const char *component = get_problem_item_content_or_NULL(problem_data, FILENAME_COMPONENT);
    const char *duphash   = get_problem_item_content_or_NULL(problem_data, FILENAME_DUPHASH);
    if (!duphash)
        error_msg_and_die(_("Essential file '%s' is missing, can't continue.."),
                          FILENAME_DUPHASH);

    if (!*duphash)
        error_msg_and_die(_("Essential file '%s' is empty, can't continue.."),
                          FILENAME_DUPHASH);

    const char *release   = get_problem_item_content_or_NULL(problem_data, FILENAME_OS_RELEASE);
    if (!release) /* Old dump dir format compat. Remove in abrt-2.1 */
        release = get_problem_item_content_or_NULL(problem_data, "release");

    struct abrt_xmlrpc *client = abrt_xmlrpc_new_client(bugzilla_xmlrpc, ssl_verify);

    log(_("Logging into Bugzilla at %s"), bugzilla_url);
    rhbz_login(client, login, password);

    log(_("Checking for duplicates"));
    char *product = NULL;
    char *version = NULL;
    parse_release_for_bz(release, &product, &version);
    free(version);

    xmlrpc_value *result;
    if (strcmp(product, "Fedora") == 0)
        result  = rhbz_search_duphash(client, component, product, duphash);
    else
        result  = rhbz_search_duphash(client, component, NULL, duphash);

    xmlrpc_value *all_bugs = rhbz_get_member("bugs", result);
    xmlrpc_DECREF(result);

    if (!all_bugs)
        error_msg_and_die(_("Missing mandatory member 'bugs'"));

    int all_bugs_size = rhbz_array_size(all_bugs);
    // When someone clones bug it has same duphash, so we can find more than 1.
    // Need to be checked if component is same.
    VERB3 log("Bugzilla has %i reports with same duphash '%s'",
              all_bugs_size, duphash);

    int bug_id = -1, dependent_bug = -1;
    struct bug_info *bz = NULL;
    if (all_bugs_size > 0)
    {
        bug_id = rhbz_bug_id(all_bugs);
        xmlrpc_DECREF(all_bugs);
        bz = rhbz_bug_info(client, bug_id);

        if (strcmp(bz->bi_product, product) != 0)
        {
            dependent_bug = bug_id;
            /* found something, but its a different product */
            free_bug_info(bz);

            xmlrpc_value *result = rhbz_search_duphash(client, component,
                                                       product, duphash);
            xmlrpc_value *all_bugs = rhbz_get_member("bugs", result);
            xmlrpc_DECREF(result);

            all_bugs_size = rhbz_array_size(all_bugs);
            if (all_bugs_size > 0)
            {
                bug_id = rhbz_bug_id(all_bugs);
                bz = rhbz_bug_info(client, bug_id);
            }
            xmlrpc_DECREF(all_bugs);
        }

    }
    free(product);

    if (all_bugs_size == 0) // Create new bug
    {
        log(_("Creating a new bug"));
        bug_id = rhbz_new_bug(client, problem_data, bug_id);

        log("Adding attachments to bug %i", bug_id);
        char bug_id_str[sizeof(int)*3 + 2];
        sprintf(bug_id_str, "%i", bug_id);

        rhbz_attachments(client, bug_id_str, problem_data);

        log(_("Logging out"));
        rhbz_logout(client);

        log("Status: NEW %s/show_bug.cgi?id=%u", bugzilla_url, bug_id);
        abrt_xmlrpc_free_client(client);
        return;
    }

    // decision based on state
    log(_("Bug is already reported: %i"), bz->bi_id);
    if ((strcmp(bz->bi_status, "CLOSED") == 0)
        && (strcmp(bz->bi_resolution, "DUPLICATE") == 0))
    {
        struct bug_info *origin;
        origin = rhbz_find_origin_bug_closed_duplicate(client, bz);
        if (origin)
        {
            free_bug_info(bz);
            bz = origin;
        }
    }

    if (strcmp(bz->bi_status, "CLOSED") != 0)
    {
        if ((strcmp(bz->bi_reporter, login) != 0)
            && (!g_list_find_custom(bz->bi_cc_list, login, (GCompareFunc)g_strcmp0)))
        {
            log(_("Add %s to CC list"), login);
            rhbz_mail_to_cc(client, bz->bi_id, login);
        }

        char *dsc = make_description_comment(problem_data);
        if (dsc)
        {
            const char *package = get_problem_item_content_or_NULL(problem_data,
                                                                   FILENAME_PACKAGE);
            const char *release = get_problem_item_content_or_NULL(problem_data,
                                                                   FILENAME_OS_RELEASE);
            if (!release) /* Old dump dir format compat. Remove in abrt-2.1 */
                release = get_problem_item_content_or_NULL(problem_data, "release");
            const char *arch = get_problem_item_content_or_NULL(problem_data,
                                                                FILENAME_ARCHITECTURE);
            const char *is_private = get_problem_item_content_or_NULL(problem_data,
                                                                      "is_private");

            char *full_dsc = xasprintf("Package: %s\n"
                                       "Architecture: %s\n"
                                       "OS Release: %s\n"
                                       "%s", package, arch, release, dsc);

            log(_("Adding new comment to bug %d"), bz->bi_id);
            free(dsc);

            int is_priv = is_private && string_to_bool(is_private);
            rhbz_add_comment(client, bz->bi_id, full_dsc, is_priv);
            free(full_dsc);
        }
    }

    log(_("Logging out"));
    rhbz_logout(client);

    log("Status: %s%s%s %s/show_bug.cgi?id=%u",
                bz->bi_status,
                bz->bi_resolution ? " " : "",
                bz->bi_resolution ? bz->bi_resolution : "",
                bugzilla_url,
                bz->bi_id);

    dd = dd_opendir(dump_dir_name, /*flags:*/ 0);
    if (dd)
    {
        char *msg = xasprintf("Bugzilla: URL=%s/show_bug.cgi?id=%u", bugzilla_url, bz->bi_id);
        add_reported_to(dd, msg);
        free(msg);
        dd_close(dd);
    }

    free_problem_data(problem_data);
    free_bug_info(bz);
    abrt_xmlrpc_free_client(client);
}

int main(int argc, char **argv)
{
    abrt_init(argv);

    map_string_h *settings = new_map_string();
    const char *dump_dir_name = ".";
    GList *conf_file = NULL;

    /* Can't keep these strings/structs static: _() doesn't support that */
    const char *program_usage_string = _(
        "\b [-v] -c CONFFILE -d DIR\n"
        "\n"
        "Reports problem to Bugzilla"
    );
    enum {
        OPT_v = 1 << 0,
        OPT_d = 1 << 1,
        OPT_c = 1 << 2,
    };
    /* Keep enum above and order of options below in sync! */
    struct options program_options[] = {
        OPT__VERBOSE(&g_verbose),
        OPT_STRING('d', NULL, &dump_dir_name, "DIR" , _("Dump directory")),
        OPT_LIST(  'c', NULL, &conf_file    , "FILE", _("Configuration file (may be given many times)")),
        OPT_END()
    };
    /*unsigned opts =*/ parse_opts(argc, argv, program_options, program_usage_string);

    export_abrt_envvars(0);

    while (conf_file)
    {
        char *fn = (char *)conf_file->data;
        VERB1 log("Loading settings from '%s'", fn);
        load_conf_file(fn, settings, /*skip key w/o values:*/ true);
        VERB3 log("Loaded '%s'", fn);
        conf_file = g_list_remove(conf_file, fn);
    }

    VERB1 log("Initializing XML-RPC library");
    xmlrpc_env env;
    xmlrpc_env_init(&env);
    xmlrpc_client_setup_global_const(&env);
    if (env.fault_occurred)
        error_msg_and_die("XML-RPC Fault: %s(%d)", env.fault_string, env.fault_code);
    xmlrpc_env_clean(&env);

    report_to_bugzilla(dump_dir_name, settings);

    free_map_string(settings);
    return 0;
}
