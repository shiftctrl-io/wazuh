/*
 * Copyright (C) 2015-2019, Wazuh Inc.
 * June 13, 2018.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation.
 */
#include "shared.h"
#include "hash_op.h"
#include "syscheck.h"
#include "syscheck_op.h"

#ifdef WIN_WHODATA

#include <winsock2.h>
#include <windows.h>
#include <aclapi.h>
#include <sddl.h>
#include <winevt.h>

#define WLIST_ALERT_THRESHOLD 80 // 80%
#define WLIST_REMOVE_MAX 10 // 10%
#define WCLIST_MAX_SIZE OS_SIZE_1024
#define WRLIST_MAX_TIME 5
#define WPOL_BACKUP_COMMAND "auditpol /backup /file:\"%s\""
#define WPOL_RESTORE_COMMAND "auditpol /restore /file:\"%s\""
#define WPOL_BACKUP_FILE "tmp\\backup-policies"
#define WPOL_NEW_FILE "tmp\\new-policies"
#define modify_criteria (FILE_WRITE_DATA | WRITE_DAC | FILE_WRITE_ATTRIBUTES)
#define criteria (DELETE | modify_criteria)
#define WHODATA_DIR_REMOVE_INTERVAL 2

// Variables whodata
static char sys_64 = 1;
static PSID everyone_sid = NULL;
static size_t ev_sid_size = 0;
static unsigned short inherit_flag = CONTAINER_INHERIT_ACE | OBJECT_INHERIT_ACE; //SUB_CONTAINERS_AND_OBJECTS_INHERIT
static EVT_HANDLE context;
static const wchar_t* event_fields[] = {
    L"Event/System/EventID",
    L"Event/EventData/Data[@Name='SubjectUserName']",
    L"Event/EventData/Data[@Name='ObjectName']",
    L"Event/EventData/Data[@Name='ProcessName']",
    L"Event/EventData/Data[@Name='ProcessId']",
    L"Event/EventData/Data[@Name='HandleId']",
    L"Event/EventData/Data[@Name='AccessMask']",
    L"Event/EventData/Data[@Name='SubjectUserSid']",
    L"Event/System/TimeCreated/@SystemTime"
};
static unsigned int fields_number = sizeof(event_fields) / sizeof(LPWSTR);
static const unsigned __int64 AUDIT_SUCCESS = 0x20000000000000;
static LPCTSTR priv = "SeSecurityPrivilege";
static int restore_policies = 0;

// Whodata function headers
void restore_sacls();
int set_privilege(HANDLE hdle, LPCTSTR privilege, int enable);
int is_valid_sacl(PACL sacl, int is_file);
unsigned long WINAPI whodata_callback(EVT_SUBSCRIBE_NOTIFY_ACTION action, __attribute__((unused)) void *_void, EVT_HANDLE event);
char *guid_to_string(GUID *guid);
int set_policies();
void set_subscription_query(wchar_t *query);
extern int wm_exec(char *command, char **output, int *exitcode, int secs, const char * add_path);
int restore_audit_policies();
void audit_restore();
int check_object_sacl(char *obj, int is_file);
int whodata_hash_add(OSHash *table, char *id, void *data, char *tag);
void notify_SACL_change(char *dir);
int whodata_path_filter(char **path);
void whodata_adapt_path(char **path);
int whodata_check_arch();
void whodata_remove_folder(OSHashNode **row, OSHashNode **node, void *data);

// Whodata list operations
whodata_event_node *whodata_list_add(char *id);
int whodata_check_removed(char *file);
void whodata_rlist_add(char *path);
void whodata_clist_remove(whodata_event_node *node);
void whodata_rlist_remove(whodata_event_node *node);
void whodata_list_set_values();
void whodata_list_remove_multiple(size_t quantity);
void send_whodata_del(whodata_evt *w_evt, char remove_hash);
int get_file_time(unsigned long long file_time_val, SYSTEMTIME *system_time);
int compare_timestamp(SYSTEMTIME *t1, SYSTEMTIME *t2);
void free_win_whodata_evt(whodata_evt *evt);
char *get_whodata_path(const short unsigned int *win_path);
void whodata_clean_rlist();

// Get volumes and paths of Windows system
int get_volume_names();
int get_drive_names(wchar_t *volume_name, char *device);
void replace_device_path(char **path);

char *guid_to_string(GUID *guid) {
    char *string_guid;
    os_calloc(40, sizeof(char *), string_guid);

    snprintf(string_guid, 40, "{%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X}",
    guid->Data1,
    guid->Data2,
    guid->Data3,
    guid->Data4[0], guid->Data4[1],
    guid->Data4[2], guid->Data4[3],guid->Data4[4], guid->Data4[5],guid->Data4[6], guid->Data4[7]);

    return string_guid;
}

int set_winsacl(const char *dir, int position) {
	DWORD result = 0;
	PACL old_sacl = NULL, new_sacl = NULL;
	PSECURITY_DESCRIPTOR security_descriptor = NULL;
    SYSTEM_AUDIT_ACE *ace = NULL;
    PVOID entry_access_it = NULL;
	HANDLE hdle;
    unsigned int i;
    ACL_SIZE_INFORMATION old_sacl_info;
    unsigned long new_sacl_size;
    int retval = 1;
    int privilege_enabled = 0;

    mdebug2("The SACL of '%s' will be configured.", dir);

	if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hdle)) {
		merror("OpenProcessToken() failed. Error '%lu'.", GetLastError());
		return 1;
	}

	if (set_privilege(hdle, priv, TRUE)) {
		merror("The privilege could not be activated. Error: '%ld'.", GetLastError());
		return 1;
	}

    privilege_enabled = 1;

	if (result = GetNamedSecurityInfo(dir, SE_FILE_OBJECT, SACL_SECURITY_INFORMATION, NULL, NULL, NULL, &old_sacl, &security_descriptor), result != ERROR_SUCCESS) {
		merror("GetNamedSecurityInfo() failed. Error '%ld'", result);
        goto end;
	}

    ZeroMemory(&old_sacl_info, sizeof(ACL_SIZE_INFORMATION));

    // Check if the sacl has what the whodata scanner needs
    switch(is_valid_sacl(old_sacl, (syscheck.wdata.dirs_status[position].object_type == WD_STATUS_FILE_TYPE) ? 1 : 0)) {
        case 0:
            mdebug1("It is necessary to configure the SACL of '%s'.", dir);
            syscheck.wdata.dirs_status[position].status |= WD_IGNORE_REST;

            // Get SACL size
            if (!GetAclInformation(old_sacl, (LPVOID)&old_sacl_info, sizeof(ACL_SIZE_INFORMATION), AclSizeInformation)) {
                merror("The size of the '%s' SACL could not be obtained.", dir);
                goto end;
            }
        break;
        case 1:
            mdebug1("It is not necessary to configure the SACL of '%s'.", dir);
            retval = 0;
            goto end;
        case 2:
            // Empty SACL
            syscheck.wdata.dirs_status[position].status |= WD_IGNORE_REST;
            old_sacl_info.AclBytesInUse = sizeof(ACL);
            break;
    }
    if (!ev_sid_size) {
        ev_sid_size = GetLengthSid(everyone_sid);
    }

    // Set the new ACL size
    new_sacl_size = old_sacl_info.AclBytesInUse + sizeof(SYSTEM_AUDIT_ACE) + ev_sid_size - sizeof(unsigned long);

    if (new_sacl = (PACL)win_alloc(new_sacl_size), !new_sacl) {
        merror("No memory could be reserved for the new SACL of '%s'.", dir);
        goto end;
    }

    if (!InitializeAcl(new_sacl, new_sacl_size, ACL_REVISION)) {
        merror("The new SACL for '%s' could not be created.", dir);
        goto end;
    }

    // If SACL is present, copy it to a new SACL
    if (old_sacl) {
        if (old_sacl_info.AceCount) {
            for (i = 0; i < old_sacl_info.AceCount; i++) {
               if (!GetAce(old_sacl, i, &entry_access_it)) {
                   merror("The ACE number %i for '%s' could not be obtained.", i, dir);
                   goto end;
               }

               if (!AddAce(new_sacl, ACL_REVISION, MAXDWORD, entry_access_it, ((PACE_HEADER)entry_access_it)->AceSize)) {
                   merror("The ACE number %i of '%s' could not be copied to the new ACL.", i, dir);
                   goto end;
               }
           }
        }
    }
    // Build the new ACE
    if (ace = (SYSTEM_AUDIT_ACE *)win_alloc(sizeof(SYSTEM_AUDIT_ACE) + ev_sid_size - sizeof(DWORD)), !ace) {
        merror("No memory could be reserved for the new ACE of '%s'.", dir);
        goto end;
    }

    ace->Header.AceType  = SYSTEM_AUDIT_ACE_TYPE;
    ace->Header.AceFlags = inherit_flag | SUCCESSFUL_ACCESS_ACE_FLAG;
    ace->Header.AceSize  = LOWORD(sizeof(SYSTEM_AUDIT_ACE) + ev_sid_size - sizeof(DWORD));
    ace->Mask            = criteria;
    if (!CopySid(ev_sid_size, &ace->SidStart, everyone_sid)) {
        goto end;
    }

    // Add the new ACE
    if (!AddAce(new_sacl, ACL_REVISION, 0, (LPVOID)ace, ace->Header.AceSize)) {
		merror("The new ACE could not be added to '%s'.", dir);
		goto end;
	}

    // Set a new ACL for the security descriptor
    if (result = SetNamedSecurityInfo((char *) dir, SE_FILE_OBJECT, SACL_SECURITY_INFORMATION, NULL, NULL, NULL, new_sacl), result != ERROR_SUCCESS) {
        merror("SetNamedSecurityInfo() failed. Error: '%lu'", result);
        goto end;
    }

	retval = 0;
end:
    if (privilege_enabled) {
        // Disable the privilege
        if (set_privilege(hdle, priv, FALSE)) {
            merror("Failed to disable the privilege. Error '%lu'.", GetLastError());
        }
    }

    if (hdle) {
        CloseHandle(hdle);
    }

    if (security_descriptor) {
        LocalFree((HLOCAL)security_descriptor);
    }

    if (old_sacl) {
        LocalFree((HLOCAL)old_sacl);
    }

    if (new_sacl) {
        LocalFree((HLOCAL)new_sacl);
    }
    if (ace) {
        LocalFree((HLOCAL)ace);
    }
    return retval;
}

int is_valid_sacl(PACL sacl, int is_file) {
    int i;
    ACCESS_ALLOWED_ACE *ace;
    SID_IDENTIFIER_AUTHORITY world_auth = {SECURITY_WORLD_SID_AUTHORITY};

    if (!everyone_sid) {
        if (!AllocateAndInitializeSid(&world_auth, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &everyone_sid)) {
            merror("Could not obtain the sid of Everyone. Error '%lu'.", GetLastError());
            return 0;
        }
    }

    if (!sacl) {
        mdebug2("No SACL found on target. A new one will be created.");
        return 2;
    }

    for (i = 0; i < sacl->AceCount; i++) {
        if (!GetAce(sacl, i, (LPVOID*)&ace)) {
            merror("Could not extract the ACE information. Error: '%lu'.", GetLastError());
            return 0;
        }

        if ((is_file || (ace->Header.AceFlags & inherit_flag)) && // Check folder and subfolders
            (ace->Header.AceFlags & SUCCESSFUL_ACCESS_ACE_FLAG) && // Check successful attemp
            ((ace->Mask & (criteria)) == criteria) && // Check write, delete, change_permissions and change_attributes permission
            (EqualSid((PSID)&ace->SidStart, everyone_sid))) { // Check everyone user
            return 1;
        }
    }
    return 0;
}

int set_privilege(HANDLE hdle, LPCTSTR privilege, int enable) {
	TOKEN_PRIVILEGES tp;
	LUID pr_uid;

	// Get the privilege UID
	if (!LookupPrivilegeValue(NULL, privilege, &pr_uid)) {
		merror("Could not find the '%s' privilege. Error: %lu", privilege, GetLastError());
		return 1;
	}

	tp.PrivilegeCount = 1;
	tp.Privileges[0].Luid = pr_uid;

	if (enable) {
		tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
	} else {
		tp.Privileges[0].Attributes = 0;
	}

    // Set the privilege to the process
	if (!AdjustTokenPrivileges(hdle, 0, &tp, sizeof(TOKEN_PRIVILEGES), (PTOKEN_PRIVILEGES)NULL, (PDWORD)NULL)) {
		merror("AdjustTokenPrivileges() failed. Error: '%lu'", GetLastError());
		return 1;
	}

    if (enable) {
        mdebug2("The '%s' privilege has been added.", privilege);
    } else {
        mdebug2("The '%s' privilege has been removed.", privilege);
    }

	return 0;
}

int run_whodata_scan() {
    wchar_t query[OS_MAXSTR];
    int result;

    if (whodata_check_arch()) {
        return 1;
    }

    // Set the signal handler to restore the policies
    atexit(audit_restore);
    // Set the system audit policies
    if (result = set_policies(), result) {
        if (result == 2) {
            mwarn("Audit policies could not be auto-configured due to the Windows version. Check if they are correct for whodata mode.");
        } else {
            mwarn("Local audit policies could not be configured.");
            return 1;
        }
    }
    // Select the interesting fields
    if (context = EvtCreateRenderContext(fields_number, event_fields, EvtRenderContextValues), !context) {
        merror("Error creating the whodata context. Error %lu.", GetLastError());
        return 1;
    }

    set_subscription_query(query);

    // Set the whodata callback
    if (!EvtSubscribe(NULL, NULL, L"Security", query,
            NULL, NULL, (EVT_SUBSCRIBE_CALLBACK)whodata_callback, EvtSubscribeToFutureEvents)) {
        merror("Event Channel subscription could not be made. Whodata scan is disabled.");
        return 1;
    }
    return 0;
}

void audit_restore() {
    restore_sacls();
    if (restore_policies) {
        restore_audit_policies();
    }
}

/* Removes added security audit policies */
void restore_sacls() {
    int i;
    PACL sacl_it;
    HANDLE hdle = NULL;
    HANDLE c_process = NULL;
    LPCTSTR priv = "SeSecurityPrivilege";
    DWORD result = 0;
    PSECURITY_DESCRIPTOR security_descriptor = NULL;
    int privilege_enabled = 0;

    c_process = GetCurrentProcess();
    if (!OpenProcessToken(c_process, TOKEN_ADJUST_PRIVILEGES, &hdle)) {
        merror("OpenProcessToken() failed restoring the SACLs. Error '%lu'.", GetLastError());
        goto end;
    }

    if (set_privilege(hdle, priv, TRUE)) {
        merror("The privilege could not be activated restoring the SACLs. Error: '%ld'.", GetLastError());
        goto end;
    }

    privilege_enabled = 1;

    for (i = 0; syscheck.dir[i] != NULL; i++) {
        if (syscheck.wdata.dirs_status[i].status & WD_IGNORE_REST) {
            sacl_it = NULL;
            if (result = GetNamedSecurityInfo(syscheck.dir[i], SE_FILE_OBJECT, SACL_SECURITY_INFORMATION, NULL, NULL, NULL, &sacl_it, &security_descriptor), result != ERROR_SUCCESS) {
                merror("GetNamedSecurityInfo() failed restoring the SACLs. Error '%ld'.", result);
                break;
            }

            // The ACE we added is in position 0
            if (!DeleteAce(sacl_it, 0)) {
                merror("DeleteAce() failed restoring the SACLs. Error '%ld'", GetLastError());
                break;
            }

            // Set the SACL
            if (result = SetNamedSecurityInfo((char *) syscheck.dir[i], SE_FILE_OBJECT, SACL_SECURITY_INFORMATION, NULL, NULL, NULL, sacl_it), result != ERROR_SUCCESS) {
                merror("SetNamedSecurityInfo() failed restoring the SACL. Error: '%lu'.", result);
                break;
            }

            if (sacl_it) {
                LocalFree((HLOCAL)sacl_it);
            }

            if (security_descriptor) {
                LocalFree((HLOCAL)security_descriptor);
            }
            mdebug1("The SACL of '%s' has been restored correctly.", syscheck.dir[i]);
        }
    }

end:
    if (privilege_enabled) {
        // Disable the privilege
        if (set_privilege(hdle, priv, FALSE)) {
            merror("Failed to disable the privilege. Error '%lu'.", GetLastError());
        }
    }

    if (hdle) {
        CloseHandle(hdle);
    }
    if (c_process) {
        CloseHandle(c_process);
    }
}

int restore_audit_policies() {
    char command[OS_SIZE_1024];
    int result_code;
    char *output;
    snprintf(command, OS_SIZE_1024, WPOL_RESTORE_COMMAND, WPOL_BACKUP_FILE);

    if (IsFile(WPOL_BACKUP_FILE)) {
        merror("There is no backup of audit policies. Policies will not be restored.");
        return 1;
    }
    // Get the current policies
    if (wm_exec(command, &output, &result_code, 5, NULL), result_code) {
        merror("Auditpol backup error: '%s'.", output);
        return 1;
    }

    return 0;
}

unsigned long WINAPI whodata_callback(EVT_SUBSCRIBE_NOTIFY_ACTION action, __attribute__((unused)) void *_void, EVT_HANDLE event) {
    unsigned int retval = 1;
    int result;
    unsigned long p_count = 0;
    unsigned long used_size;
    PEVT_VARIANT buffer = NULL;
    whodata_evt *w_evt;
    short event_id;
    char *user_name = NULL;
    char *path = NULL;
    char *process_name = NULL;
    unsigned __int64 process_id;
    unsigned __int64 handle_id;
    char *user_id = NULL;
    char is_directory;
    char ignore_remove_event;
    unsigned int mask;
    int position;
    whodata_directory *w_dir;
    SYSTEMTIME system_time;
    syscheck_node *s_node;

    if (action == EvtSubscribeActionDeliver) {
        char hash_id[21];

        // Extract the necessary memory size
        EvtRender(context, event, EvtRenderEventValues, 0, NULL, &used_size, &p_count);
        // We may be taking more memory than we need to
		buffer = (PEVT_VARIANT)malloc(used_size);

        if (!EvtRender(context, event, EvtRenderEventValues, used_size, buffer, &used_size, &p_count)) {
			merror("Error rendering the event. Error %lu.", GetLastError());
            goto clean;
		}

        if (fields_number != p_count) {
			merror("Invalid number of rendered parameters.");
            goto clean;
        }

        if (buffer[0].Type != EvtVarTypeUInt16) {
            merror(INV_WDATA_PAR, buffer[0].Type, "event_id");
            goto clean;
        }
        event_id = buffer[0].Int16Val;

        // Check types
        if (buffer[2].Type != EvtVarTypeString) {
            if (event_id == 4658 || event_id == 4660) {
                path = NULL;
            } else {
                merror(INV_WDATA_PAR, buffer[2].Type, "path");
                goto clean;
            }
        }  else {
            if (path = get_whodata_path(buffer[2].XmlVal), !path) {
                goto clean;
            }

            // Replace in string path \device\harddiskvolumeX\ by drive letter
            replace_device_path(&path);

            str_lowercase(path);
            if (whodata_path_filter(&path)) {
                goto clean;
            }
        }

        if (buffer[1].Type != EvtVarTypeString) {
            mwarn(INV_WDATA_PAR, buffer[1].Type, "user_name");
            user_name = NULL;
        } else {
            user_name = convert_windows_string(buffer[1].XmlVal);
        }

        if (buffer[3].Type != EvtVarTypeString) {
            mwarn(INV_WDATA_PAR, buffer[3].Type, "process_name");
            process_name = NULL;
        } else {
            process_name = convert_windows_string(buffer[3].XmlVal);
        }

        // In 32-bit Windows we find EvtVarTypeSizeT
        if (buffer[4].Type != EvtVarTypeHexInt64) {
            if (buffer[4].Type == EvtVarTypeSizeT) {
                process_id = (unsigned __int64) buffer[4].SizeTVal;
            } else if (buffer[4].Type == EvtVarTypeHexInt32) {
                process_id = (unsigned __int64) buffer[4].UInt32Val;
            } else {
                mwarn(INV_WDATA_PAR, buffer[4].Type, "process_id");
                process_id = 0;
            }
        } else {
            process_id = buffer[4].UInt64Val;
        }

        // In 32-bit Windows we find EvtVarTypeSizeT or EvtVarTypeHexInt32
        if (buffer[5].Type != EvtVarTypeHexInt64) {
            if (buffer[5].Type == EvtVarTypeSizeT) {
                handle_id = (unsigned __int64) buffer[5].SizeTVal;
            } else if (buffer[5].Type == EvtVarTypeHexInt32) {
                handle_id = (unsigned __int64) buffer[5].UInt32Val;
            } else {
                merror(INV_WDATA_PAR, buffer[5].Type, "handle_id");
                goto clean;
            }
        } else {
            handle_id = buffer[5].UInt64Val;
        }

        if (buffer[6].Type != EvtVarTypeHexInt32) {
            if (event_id == 4658 || event_id == 4660) {
                mask = 0;
            } else {
                merror(INV_WDATA_PAR, buffer[6].Type, "mask");
                goto clean;
            }
        } else {
            mask = buffer[6].UInt32Val;
        }

        whodata_clean_rlist();

        if (buffer[7].Type != EvtVarTypeSid) {
            mwarn(INV_WDATA_PAR, buffer[7].Type, "user_id");
            user_id = NULL;
        } else if (!ConvertSidToStringSid(buffer[7].SidVal, &user_id)) {
            mdebug1("Invalid identifier for user '%s'", user_name);
            goto clean;
        }
        snprintf(hash_id, 21, "%llu", handle_id);
        switch(event_id) {
            // Open fd
            case 4656:
                is_directory = 0;
                ignore_remove_event = 0;
                position = -1;

                if (!path) {
                    goto clean;
                }
                // Check if it is a known file
                if (s_node = OSHash_Get_ex(syscheck.fp, path), !s_node) {
                    int device_type;
                    if (strchr(path, ':')) {
                        if (position = find_dir_pos(path, 1, CHECK_WHODATA, 1), position < 0) {
                            // Discard the file or directory if its monitoring has not been activated
                            mdebug2("'%s' is discarded because its monitoring is not activated.", path);
                            whodata_hash_add(syscheck.wdata.ignored_paths, path, &fields_number, "ignored");
                            break;
                        } else {
                            // The file or directory is new and has to be notified
                        }

                        if (device_type = check_path_type(path), device_type == 2) { // If it is an existing directory, check_path_type returns 2
                            is_directory = 1;
                        } else if (device_type == 0) {
                            // If the device could not be found, it was monitored by Syscheck,
                            // has not recently been removed,
                            // and had never been entered in the hash table before,
                            // we can deduce that it is a removed directory
                            if (mask & DELETE && !whodata_check_removed(path)) {
                                mdebug2("Removed folder event received for '%s'.", path);
                                is_directory = 1;
                            } else {
                                break;
                            }
                        } else {
                            // It is an existing file
                        }
                    } else {
                        mdebug2("Uncontrolled whodata event on '%s'.", path);
                        break;
                    }
                } else {
                    if (s_node->dir_position < 0) {
                        merror("The '%s' file does not have an associated directory.", path);
                        goto clean;
                    }

                    // Check if the file belongs to a directory that has been transformed to real-time
                    if (!(syscheck.wdata.dirs_status[s_node->dir_position].status & WD_CHECK_WHODATA)) {
                        mdebug2("The monitoring of '%s' in whodata mode has been canceled. Added to the ignore list.", path);
                        whodata_hash_add(syscheck.wdata.ignored_paths, path, &fields_number, "ignored");
                        goto clean;
                    }
                    // If the file or directory is already in the hash table, it is not necessary to set its position
                    if (check_path_type(path) == 2) {
                        is_directory = 1;
                    } else {
                        // The file exists at this points. We will only notify its deletion if the event expressly indicates it
                        ignore_remove_event = 1;
                    }
                }

                os_calloc(1, sizeof(whodata_evt), w_evt);
                w_evt->user_name = user_name;
                w_evt->user_id = user_id;
                if (!is_directory) {
                    w_evt->path = path;
                    path = NULL;
                } else {
                    // The directory path will be saved in 4663 event
                    w_evt->path = NULL;
                }

                if (position > -1) {
                    w_evt->dir_position = position;
                }
                w_evt->process_name = process_name;
                w_evt->process_id = process_id;
                w_evt->mask = 0;
                w_evt->scan_directory = is_directory;
                w_evt->ignore_remove_event = ignore_remove_event;
                w_evt->deleted = 0;
                w_evt->ignore_not_exist = 0;
                w_evt->ppid = -1;
                w_evt->wnode = whodata_list_add(strdup(hash_id));

                user_name = NULL;
                user_id = NULL;
                process_name = NULL;
add_whodata_evt:
                if (result = whodata_hash_add(syscheck.wdata.fd, hash_id, w_evt, "whodata"), result != 2) {
                    if (result == 1) {
                        mdebug1("The handler ('%s') will be updated.", hash_id);
                        whodata_evt *w_evtdup;
                        if (w_evtdup = OSHash_Delete_ex(syscheck.wdata.fd, hash_id), w_evtdup) {
                            free_win_whodata_evt(w_evtdup);
                            goto add_whodata_evt;
                        } else {
                            merror("The handler '%s' could not be removed from the whodata hash table.", hash_id);
                        }
                    }
                    free_win_whodata_evt(w_evt);
                    retval = 1;
                    goto clean;
                }
            break;
            // Write fd
            case 4663:
                // Check if the mask is relevant
                if (mask) {
                    if (w_evt = OSHash_Get(syscheck.wdata.fd, hash_id), w_evt) {
                        w_evt->mask |= mask;
                        // Check if it is a rename or copy event
                        if (w_evt->scan_directory) {
                            if (mask & FILE_WRITE_DATA) {
                                if (w_dir = OSHash_Get_ex(syscheck.wdata.directories, path), w_dir) {
                                    // Get the event time
                                    if (buffer[8].Type != EvtVarTypeFileTime) {
                                        merror(INV_WDATA_PAR, buffer[8].Type, "event_time");
                                        w_evt->scan_directory = 2;
                                        goto clean;
                                    }
                                    if (!get_file_time(buffer[8].FileTimeVal, &system_time)) {
                                        merror("Could not get the time of the event whose handler is '%llu'.", handle_id);
                                        goto clean;
                                    }

                                    if (!compare_timestamp(&w_dir->timestamp, &system_time)) {
                                        mdebug2("The '%s' directory has been scanned at 'd'. It does not need to do it again.", path);
                                        w_evt->scan_directory = 3;
                                        break;
                                    }
                                    mdebug2("New files have been detected in the '%s' directory after the last scan.", path);
                                } else {
                                    // Check if is a valid directory
                                    if (position = find_dir_pos(path, 1, CHECK_WHODATA, 1), position < 0) {
                                        mdebug2("The '%s' directory has been discarded because it is not being monitored in whodata mode.", path);
                                        w_evt->scan_directory = 2;
                                        break;
                                    }
                                    os_calloc(1, sizeof(whodata_directory), w_dir);
                                    memset(&w_dir->timestamp, 0, sizeof(SYSTEMTIME));
                                    w_dir->position = position;

                                    if (result = whodata_hash_add(syscheck.wdata.directories, path, w_dir, "directories"), result != 2) {
                                        w_evt->scan_directory = 2;
                                        free(w_dir);
                                        break;
                                    } else {
                                        mdebug2("New files have been detected in the '%s' directory and will be scanned.", path);
                                    }
                                }
                                w_evt->path = path;
                                path = NULL;
                            } else if (mask & DELETE) {
                                // The directory has been removed
                                w_evt->path = path;
                                path = NULL;
                            }
                        }
                    } else {
                        // The file was opened before Wazuh started Syscheck.
                    }
                }
            break;
            // Deleted file
            case 4660:
                if (w_evt = OSHash_Get(syscheck.wdata.fd, hash_id), w_evt) {
                    // The file has been deleted
                    w_evt->deleted = 1;
                } else {
                    // The file was opened before Wazuh started Syscheck.
                }
            break;
            // Close fd
            case 4658:
                if (w_evt = OSHash_Delete_ex(syscheck.wdata.fd, hash_id), w_evt) {
                    unsigned int mask = w_evt->mask;
                    if (!w_evt->scan_directory) {
                        if (w_evt->deleted) {
                            // Check if the file has been deleted
                            w_evt->ignore_remove_event = 0;
                            send_whodata_del(w_evt, 1);
                        } else if (mask & DELETE) {
                            // The file has been moved or renamed
                            w_evt->ignore_remove_event = 0;
                            send_whodata_del(w_evt, 1);
                        } else if (mask & modify_criteria) {
                            // Check if the file has been modified
                            realtime_checksumfile(w_evt->path, NULL, w_evt);
                        } else {
                            // At this point the file can be created
                            realtime_checksumfile(w_evt->path, NULL, w_evt);
                        }
                    } else if (w_evt->scan_directory == 1) { // Directory scan has been aborted if scan_directory is 2
                        if (mask & DELETE) {
                            static char *last_mdir = NULL;
                            static time_t last_mdir_tm = 0;
                            time_t now = time(NULL);

                            // We will not process deletion events on the same directory in less than WHODATA_DIR_REMOVE_INTERVAL seconds
                            if (!last_mdir || strcmp(last_mdir, w_evt->path) ||
                                last_mdir_tm + WHODATA_DIR_REMOVE_INTERVAL < now) {
                                if (w_evt->path) {
                                    char *dir_path;
                                    char *saved_path;

                                    saved_path = w_evt->path;

                                    os_calloc(strlen(w_evt->path) + 2, sizeof(char), dir_path);
                                    snprintf(dir_path, strlen(w_evt->path) + 2, "%s\\", w_evt->path);
                                    w_evt->path = dir_path;

                                    // Notify removed files
                                    mdebug1("Directory '%s' has been moved or removed.", dir_path);
                                    OSHash_It_ex(syscheck.fp, (void *) w_evt, whodata_remove_folder);
                                    free(dir_path);
                                    w_evt->path = saved_path;

                                    // Find new files
                                    read_dir(syscheck.dir[w_evt->dir_position], NULL, w_evt->dir_position, w_evt, syscheck.recursion_level[w_evt->dir_position], 0);

                                    last_mdir_tm = now;
                                    free(last_mdir);
                                    os_strdup(w_evt->path, last_mdir);
                                } else {
                                    mdebug2("Uncontrolled removed folder event.");
                                }
                            } else {
                                mdebug2("Ignoring removing event for '%s' directory.", w_evt->path);
                            }
                        } else if ((mask & FILE_WRITE_DATA) && w_evt->path && (w_dir = OSHash_Get(syscheck.wdata.directories, w_evt->path))) {
                            // Check that a new file has been added
                            GetSystemTime(&w_dir->timestamp);
                            int pos;
                            if (pos = find_dir_pos(w_evt->path, 1, CHECK_WHODATA, 1), pos >= 0) {
                                int diff = fim_find_child_depth(syscheck.dir[pos], w_evt->path);
                                int depth = syscheck.recursion_level[pos] - diff;
                                read_dir(w_evt->path, NULL, pos, w_evt, depth, 0);
                            }

                            mdebug1("The '%s' directory has been scanned after detecting event of new files.", w_evt->path);
                        } else {
                            mdebug2("The '%s' directory has not been scanned because no new files have been detected. Mask: '%x'", w_evt->path, w_evt->mask);
                        }
                    } else if (w_evt->scan_directory == 2) {
                        mdebug1("Scanning of the '%s' directory is aborted because something has gone wrong.", w_evt->path);
                    }
                    free_win_whodata_evt(w_evt);
                } else {
                    // The file was opened before Wazuh started Syscheck.
                }
            break;
            default:
                merror("Invalid EventID. The whodata cannot be extracted.");
                retval = 1;
                goto clean;
        }
    }
    retval = 0;
clean:
    free(user_name);
    free(path);
    free(process_name);
    if (user_id) {
        LocalFree(user_id);
    }
    if (buffer) {
        free(buffer);
    }
    return retval;
}

int whodata_audit_start() {
    // Set the hash table of ignored paths
    if (syscheck.wdata.ignored_paths = OSHash_Create(), !syscheck.wdata.ignored_paths) {
        return 1;
    }
    // Set the hash table of directories
    if (syscheck.wdata.directories = OSHash_Create(), !syscheck.wdata.directories) {
        return 1;
    }
    // Set the hash table of file descriptors
    // We assume that its default value is 1024
    if (syscheck.wdata.fd = OSHash_Create(), !syscheck.wdata.fd) {
        return 1;
    }

    OSHash_SetFreeDataPointer(syscheck.wdata.fd, (void (*)(void *))free_win_whodata_evt);

    memset(&syscheck.w_clist, 0, sizeof(whodata_event_list));
    memset(&syscheck.w_rlist, 0, sizeof(whodata_event_list));
    whodata_list_set_values();

    minfo("Analyzing Windows volumes");
    get_volume_names();

    return 0;
}

long unsigned int WINAPI state_checker(__attribute__((unused)) void *_void) {
    int i;
    int exists;
    whodata_dir_status *d_status;
    SYSTEMTIME utc;
    int interval;

    if (!syscheck.wdata.interval_scan) {
        interval = WDATA_DEFAULT_INTERVAL_SCAN;
    } else {
        interval = syscheck.wdata.interval_scan;
    }

    mdebug1("Checking thread set to %d seconds.", interval);

    while (1) {
        for (i = 0; syscheck.dir[i]; i++) {
            exists = 0;
            d_status = &syscheck.wdata.dirs_status[i];

            if (!(syscheck.wdata.dirs_status[i].status & WD_CHECK_WHODATA)) {
                // It is not whodata
                continue;
            }

            switch (check_path_type(syscheck.dir[i])) {
                case 0:
                    // Unknown device type or does not exist
                    exists = 0;
                break;
                case 1:
                    exists = 1;
                    syscheck.wdata.dirs_status[i].object_type = WD_STATUS_FILE_TYPE;
                break;
                case 2:
                    exists = 1;
                    syscheck.wdata.dirs_status[i].object_type = WD_STATUS_DIR_TYPE;
                break;

            }

            if (exists) {
                if (!(d_status->status & WD_STATUS_EXISTS)) {
                    minfo("'%s' has been re-added. It will be monitored in Whodata mode.", syscheck.dir[i]);
                    if (set_winsacl(syscheck.dir[i], i)) {
                        merror("Unable to add directory to whodata monitoring: '%s'.", syscheck.dir[i]);
                        continue;
                    }
                    d_status->status |= WD_STATUS_EXISTS;
                } else {
                    if (get_creation_date(syscheck.dir[i], &utc)) {
                        merror("The creation date for '%s' could not be extracted.", syscheck.dir[i]);
                        continue;
                    }

                    if (compare_timestamp(&d_status->last_check, &utc)) {
                        mdebug1("'%s' has been deleted and added after the last scan.", syscheck.dir[i]);
                        if (set_winsacl(syscheck.dir[i], i)) {
                            merror("Unable to add directory to whodata monitoring: '%s'.", syscheck.dir[i]);
                            continue;
                        }
                    } else {
                        if (check_object_sacl(syscheck.dir[i], (d_status->object_type == WD_STATUS_FILE_TYPE) ? 1 : 0)) {
                            minfo("The SACL of '%s' has been modified and it is not valid for the Whodata mode. Real-time mode will be activated for this file.", syscheck.dir[i]);
                            // Mark the directory to prevent its children from sending partial whodata alerts
                            syscheck.wdata.dirs_status[i].status |= WD_CHECK_REALTIME;
                            syscheck.wdata.dirs_status[i].status &= ~WD_CHECK_WHODATA;
                            // Removes CHECK_WHODATA from directory properties to prevent from being found in the whodata callback for Windows (find_dir_pos)
                            syscheck.opts[i] = syscheck.opts[i] & ~CHECK_WHODATA;
                            // Mark it to prevent the restoration of its SACL
                            syscheck.wdata.dirs_status[i].status &= ~WD_IGNORE_REST;
                            notify_SACL_change(syscheck.dir[i]);
                            continue;
                        } else {
                            // The SACL is valid
                        }
                    }
                }
            } else {
                minfo("'%s' has been deleted. It will not be monitored in Whodata mode.", syscheck.dir[i]);
                d_status->status &= ~WD_STATUS_EXISTS;
                d_status->object_type = WD_STATUS_UNK_TYPE;
            }
            // Set the timestamp
            GetSystemTime(&d_status->last_check);
        }
        sleep(interval);
    }

    return 0;
}

whodata_event_node *whodata_list_add(char *id) {
    whodata_event_node *node = NULL;
    if (syscheck.w_clist.current_size < syscheck.w_clist.max_size) {
        if (!syscheck.w_clist.alerted && syscheck.w_clist.alert_threshold < syscheck.w_clist.current_size) {
            syscheck.w_clist.alerted = 1;
            mwarn("Whodata events queue for Windows has more than %d elements.", syscheck.w_clist.alert_threshold);
        }
    } else {
        mdebug1("Whodata events queue for Windows is full. Removing the first %d...", syscheck.w_clist.max_remove);
        whodata_list_remove_multiple(syscheck.w_clist.max_remove);
    }
    os_calloc(sizeof(whodata_event_node), 1, node);
    if (syscheck.w_clist.last) {
        node->next = NULL;
        node->prev = syscheck.w_clist.last;
        syscheck.w_clist.last = node;
    } else {
        node->next = node->prev = NULL;
        syscheck.w_clist.last = syscheck.w_clist.first = node;
    }
    node->id = id;
    syscheck.w_clist.current_size++;

    return node;
}

void whodata_list_remove_multiple(size_t quantity) {
    size_t i;
    whodata_evt *w_evt;
    for (i = 0; i < quantity && syscheck.w_clist.first; i++) {
        if (w_evt = OSHash_Delete_ex(syscheck.wdata.fd, syscheck.w_clist.first->id), w_evt) {
            free_whodata_event(w_evt);
        }
        whodata_clist_remove(syscheck.w_clist.first);
    }
    mdebug1("%d events have been deleted from the whodata list.", quantity);
}

void whodata_clist_remove(whodata_event_node *node) {
    if (!(node->next || node->prev)) {
        syscheck.w_clist.first = syscheck.w_clist.last = NULL;
    } else {
        if (node->next) {
            if (node->prev) {
                node->next->prev = node->prev;
            } else {
                node->next->prev = NULL;
                syscheck.w_clist.first = node->next;
            }
        }

        if (node->prev) {
            if (node->next) {
                node->prev->next = node->next;
            } else {
                node->prev->next = NULL;
                syscheck.w_clist.last = node->prev;
            }
        }
    }

    free(node->id);
    free(node);

    syscheck.w_clist.current_size--;

    if (syscheck.w_clist.alerted && syscheck.w_clist.alert_threshold > syscheck.w_clist.current_size) {
        syscheck.w_clist.alerted = 0;
    }
}

void whodata_rlist_remove(whodata_event_node *node) {
    if (!(node->next || node->prev)) {
        syscheck.w_rlist.first = syscheck.w_rlist.last = NULL;
    } else {
        if (node->next) {
            if (node->prev) {
                node->next->prev = node->prev;
            } else {
                node->next->prev = NULL;
                syscheck.w_rlist.first = node->next;
            }
        }

        if (node->prev) {
            if (node->next) {
                node->prev->next = node->next;
            } else {
                node->prev->next = NULL;
                syscheck.w_rlist.last = node->prev;
            }
        }
    }

    free(node->id);
    free(node);
}

void whodata_list_set_values() {
    // Cached events list
    syscheck.w_clist.max_size = WCLIST_MAX_SIZE;
    syscheck.w_clist.max_remove = syscheck.w_clist.max_size * WLIST_REMOVE_MAX * 0.01;
    syscheck.w_clist.alert_threshold = syscheck.w_clist.max_size * WLIST_ALERT_THRESHOLD * 0.01;
    mdebug1("Whodata event queue values for Windows -> max_size:'%d' | max_remove:'%d' | alert_threshold:'%d'.",
    syscheck.w_clist.max_size, syscheck.w_clist.max_remove, syscheck.w_clist.alert_threshold);

    // Removed events list
    syscheck.w_rlist.queue_time = WRLIST_MAX_TIME;
}

void send_whodata_del(whodata_evt *w_evt, char remove_hash) {
    static char del_msg[PATH_MAX + OS_SIZE_6144 + 6];
    static char wd_sum[OS_SIZE_6144 + 1];
    syscheck_node *s_node;
    int pos = w_evt->dir_position;

    if (remove_hash) {
        // Remove the file from the syscheck hash table
        if (s_node = OSHash_Delete_ex(syscheck.fp, w_evt->path), !s_node) {
            return;
        }

        free(s_node->checksum);
        free(s_node);
    }

    if (extract_whodata_sum(w_evt, wd_sum, OS_SIZE_6144)) {
        merror("The whodata sum for '%s' file could not be included in the alert as it is too large.", w_evt->path);
    }

    /* Find tag if defined for this file */
    if (pos < 0) {
        pos = find_dir_pos(w_evt->path, 1, 0, 0);
    }

    snprintf(del_msg, PATH_MAX + OS_SIZE_6144 + 6, "-1!%s:%s: %s", wd_sum, syscheck.tag[pos] ? syscheck.tag[pos] : "", w_evt->path);
    send_syscheck_msg(del_msg);
    whodata_rlist_add(w_evt->path);
}

int set_policies() {
    char *output = NULL;
    int result_code = 0;
    FILE *f_backup = NULL;
    FILE *f_new = NULL;
    char buffer[OS_MAXSTR];
    char command[OS_SIZE_1024];
    int retval = 1;
    static const char *WPOL_FILE_SYSTEM_SUC = ",System,File System,{0CCE921D-69AE-11D9-BED3-505054503030},,,1\n";
    static const char *WPOL_HANDLE_SUC = ",System,Handle Manipulation,{0CCE9223-69AE-11D9-BED3-505054503030},,,1\n";

    if (!IsFile(WPOL_BACKUP_FILE) && remove(WPOL_BACKUP_FILE)) {
        merror("'%s' could not be removed: %s (%d).", WPOL_BACKUP_FILE, strerror(errno), errno);
        goto end;
    }

    snprintf(command, OS_SIZE_1024, WPOL_BACKUP_COMMAND, WPOL_BACKUP_FILE);

    // Get the current policies
    if (wm_exec(command, &output, &result_code, 5, NULL), result_code) {
        retval = 2;
        goto end;
    }

    free(output);
    output = NULL;

    if (f_backup = fopen (WPOL_BACKUP_FILE, "r"), !f_backup) {
        merror("'%s' could not be opened: %s (%d).", WPOL_BACKUP_FILE, strerror(errno), errno);
        goto end;
    }
    if (f_new = fopen (WPOL_NEW_FILE, "w"), !f_new) {
        merror("'%s' could not be removed: %s (%d).", WPOL_NEW_FILE, strerror(errno), errno);
        goto end;
    }

    // Copy the policies
    while (fgets(buffer, OS_MAXSTR - 60, f_backup)) {
        fprintf(f_new, buffer);
    }

    // Add the new policies
    fprintf(f_new, WPOL_FILE_SYSTEM_SUC);
    fprintf(f_new, WPOL_HANDLE_SUC);

    fclose(f_new);

    snprintf(command, OS_SIZE_1024, WPOL_RESTORE_COMMAND, WPOL_NEW_FILE);

    // Set the new policies
    if (wm_exec(command, &output, &result_code, 5, NULL), result_code) {
        retval = 2;
        goto end;
    }

    retval = 0;
    restore_policies = 1;
end:
    free(output);
    if (f_backup) {
        fclose(f_backup);
    }
    return retval;
}

void set_subscription_query(wchar_t *query) {
    snwprintf(query, OS_MAXSTR, L"Event[ System[band(Keywords, %llu)] " \
                                    "and " \
                                        "( " \
                                            "( " \
                                                "( " \
                                                    "EventData/Data[@Name='ObjectType'] = 'File' " \
                                                ") " \
                                            "and " \
                                                "( " \
                                                    "(  " \
                                                        "System/EventID = 4656 " \
                                                    "or " \
                                                        "System/EventID = 4663 " \
                                                    ") " \
                                                "and " \
                                                    "( " \
                                                        "EventData[band(Data[@Name='AccessMask'], %lu)] " \
                                                    ") " \
                                                ") " \
                                            ") " \
                                        "or " \
                                            "System/EventID = 4658 " \
                                        "or " \
                                            "System/EventID = 4660 " \
                                        ") " \
                                    "]",
            AUDIT_SUCCESS, // Only successful events
            criteria); // For 4663 and 4656 events need write, delete, change_attributes or change_permissions accesss
}

int get_file_time(unsigned long long file_time_val, SYSTEMTIME *system_time) {
    FILETIME file_time;
    file_time.dwHighDateTime = (DWORD)((file_time_val >> 32) & 0xFFFFFFFF);
    file_time.dwLowDateTime = (DWORD)(file_time_val & 0xFFFFFFFF);
    return FileTimeToSystemTime(&file_time, system_time);
}

int compare_timestamp(SYSTEMTIME *t1, SYSTEMTIME *t2) {
    if (t1->wYear > t2->wYear) {
        return 0;
    } else if (t1->wYear < t2->wYear) {
        return 1;
    }

    if (t1->wMonth > t2->wMonth) {
        return 0;
    } else if (t1->wMonth < t2->wMonth) {
        return 1;
    }

    if (t1->wDay > t2->wDay) {
        return 0;
    } else if (t1->wDay < t2->wDay) {
        return 1;
    }

    if (t1->wHour > t2->wHour) {
        return 0;
    } else if (t1->wHour < t2->wHour) {
        return 1;
    }

    if (t1->wMinute > t2->wMinute) {
        return 0;
    } else if (t1->wMinute < t2->wMinute) {
        return 1;
    }

    if (t1->wSecond > t2->wSecond) {
        return 0;
    } else if (t1->wSecond < t2->wSecond) {
        return 1;
    }

    return 1;
}

void free_win_whodata_evt(whodata_evt *evt) {
    whodata_clist_remove(evt->wnode);
    free_whodata_event(evt);
}

int check_object_sacl(char *obj, int is_file) {
    HANDLE hdle = NULL;
    PACL sacl = NULL;
    int retval = 1;
    PSECURITY_DESCRIPTOR security_descriptor = NULL;
    long int result;
    int privilege_enabled = 0;

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hdle)) {
        merror("OpenProcessToken() failed. Error '%lu'.", GetLastError());
        return 1;
    }

    if (set_privilege(hdle, priv, TRUE)) {
        merror("The privilege could not be activated. Error: '%ld'.", GetLastError());
        goto end;
    }

    privilege_enabled = 1;
    if (result = GetNamedSecurityInfo(obj, SE_FILE_OBJECT, SACL_SECURITY_INFORMATION, NULL, NULL, NULL, &sacl, &security_descriptor), result != ERROR_SUCCESS) {
        merror("GetNamedSecurityInfo() failed. Error '%ld'", result);
        goto end;
    }

    if (is_valid_sacl(sacl, is_file) == 1) {
        // Is a valid SACL
        retval = 0;
    }

end:
    if (privilege_enabled) {
        // Disable the privilege
        if (set_privilege(hdle, priv, FALSE)) {
            merror("Failed to disable the privilege. Error '%lu'.", GetLastError());
        }
    }
    if (hdle) {
        CloseHandle(hdle);
    }
    if (security_descriptor) {
        LocalFree((HLOCAL)security_descriptor);
    }
    if (sacl) {
        LocalFree((HLOCAL)sacl);
    }
    return retval;
}

int whodata_hash_add(OSHash *table, char *id, void *data, char *tag) {
    int result;

    if (result = OSHash_Add_ex(table, id, data), result != 2) {
        if (!result) {
            merror("The event could not be added to the %s hash table. Target: '%s'.", tag, id);
        } else if (result == 1) {
            merror("The event could not be added to the %s hash table because it is duplicated. Target: '%s'.", tag, id);
        }
    }

    return result;
}

void notify_SACL_change(char *dir) {
    char msg_alert[OS_SIZE_1024 + 1];
    snprintf(msg_alert, OS_SIZE_1024, "ossec: Audit: The SACL of '%s' has been modified and can no longer be scanned in whodata mode.", dir);
    SendMSG(syscheck.queue, msg_alert, "syscheck", LOCALFILE_MQ);
}

int get_volume_names() {
    char *convert_device;
    char *convert_volume;
    wchar_t device_name[MAX_PATH] = L"";
    wchar_t volume_name[MAX_PATH] = L"";
    HANDLE fh = INVALID_HANDLE_VALUE;
    unsigned long char_count = 0;
    size_t index = 0;
    size_t success = -1;
    size_t win_error = ERROR_SUCCESS;

    // Enumerate all volumes in the system.
    fh = FindFirstVolumeW(volume_name, ARRAYSIZE(volume_name));

    if (fh == INVALID_HANDLE_VALUE) {
        win_error = GetLastError();
        mwarn("FindFirstVolumeW failed (%u)'%s'", win_error, strerror(win_error));
        FindVolumeClose(fh);
        fh = INVALID_HANDLE_VALUE;
        return success;
    }

    os_calloc(MAX_PATH, sizeof(char), convert_volume);
    os_calloc(MAX_PATH, sizeof(char), convert_device);

    // The loop ends when there are no more volumes
    while (1) {
        //  Skip the \\?\ prefix and remove the trailing backslash.
        index = wcslen(volume_name) - 1;

        // Convert volume_name
        wcstombs(convert_volume, volume_name, ARRAYSIZE(volume_name));

        if (volume_name[0]     != L'\\' ||
            volume_name[1]     != L'\\' ||
            volume_name[2]     != L'?'  ||
            volume_name[3]     != L'\\' ||
            volume_name[index] != L'\\')
        {
            win_error = ERROR_BAD_PATHNAME;
            mwarn("Find Volume returned a bad path: %s", convert_volume);
            break;
        }

        // QueryDosDeviceW does not allow a trailing backslash,
        // so temporarily remove it.
        volume_name[index] = '\0';
        char_count = QueryDosDeviceW(&volume_name[4], device_name, ARRAYSIZE(device_name));
        volume_name[index] = '\\';

        if (char_count == 0) {
            win_error = GetLastError();
            mwarn("QueryDosDeviceW failed (%u)'%s'", win_error, strerror(win_error));
            break;
        }

        // Convert device name
        wcstombs(convert_device, device_name, ARRAYSIZE(device_name));
        // Get all drive letters
        get_drive_names(volume_name, convert_device);

        // Move on to the next volume.
        success = FindNextVolumeW(fh, volume_name, ARRAYSIZE(volume_name));

        if (!success) {
            win_error = GetLastError();

            if (win_error != ERROR_NO_MORE_FILES) {
                mwarn("FindNextVolumeW failed (%u)'%s'", win_error, strerror(win_error));
                break;
            }

            // Finished iterating, through all the volumes.
            win_error = ERROR_SUCCESS;
            success = 0;
            break;
        }
    }

    FindVolumeClose(fh);
    fh = INVALID_HANDLE_VALUE;

    os_free(convert_device);
    os_free(convert_volume);

    return success;
}

int get_drive_names(wchar_t *volume_name, char *device) {
    char *convert_name;
    wchar_t *names = NULL;
    wchar_t *nameit = NULL;
    unsigned long char_count = MAX_PATH + 1;
    unsigned int device_it;
    size_t success = -1;
    size_t retval = -1;

    while (1) {
        // Allocate a buffer to hold the paths.
        os_calloc(MAX_PATH, sizeof(wchar_t *), names);

        // Obtain all of the paths for this volume.
        success = GetVolumePathNamesForVolumeNameW(
            volume_name, names, char_count, &char_count
            );

        if (success) {
            retval = 0;
            break;
        }

        if (retval = GetLastError(), retval != ERROR_MORE_DATA) {
            mwarn("GetVolumePathNamesForVolumeNameW (%u)'%s'", retval, strerror(retval));
            break;
        }

        //  Try again with the new suggested size.
        os_free(names);
        names = NULL;
    }

    if (success) {
        // Save information in FIM whodata structure
        os_calloc(MAX_PATH, sizeof(char), convert_name);

        for (nameit = names; nameit[0] != L'\0'; nameit += wcslen(nameit) + 1) {
            wcstombs(convert_name, nameit, ARRAYSIZE(nameit));
            mdebug1("Device '%s' associated with the mounting point '%s'", device, convert_name);

            if(syscheck.wdata.device) {
                device_it = 0;

                while(syscheck.wdata.device[device_it]) {
                    device_it++;
                }

                os_realloc(syscheck.wdata.device,
                        (device_it + 2) * sizeof(char*),
                        syscheck.wdata.device);
                os_strdup(device, syscheck.wdata.device[device_it]);
                syscheck.wdata.device[device_it + 1] = NULL;

                os_realloc(syscheck.wdata.drive,
                        (device_it + 2) * sizeof(char*),
                        syscheck.wdata.drive);
                os_strdup(convert_name, syscheck.wdata.drive[device_it]);
                syscheck.wdata.drive[device_it + 1] = NULL;

            } else {
                os_calloc(2, sizeof(char*), syscheck.wdata.device);
                os_strdup(device, syscheck.wdata.device[0]);
                syscheck.wdata.device[1] = NULL;

                os_calloc(2, sizeof(char*), syscheck.wdata.drive);
                os_strdup(convert_name, syscheck.wdata.drive[0]);
                syscheck.wdata.drive[1] = NULL;
            }
        }
        os_free(convert_name);
    }
    os_free(names);

    return 0;
}

void replace_device_path(char **path) {
    char *new_path;
    unsigned int iterator = 0;

    if (**path != '\\') {
        return;
    }

    while (syscheck.wdata.device[iterator]) {
        size_t dev_size = strlen(syscheck.wdata.device[iterator]);

        mdebug2("Find device '%s' in path '%s'", syscheck.wdata.device[iterator], *path);

        if (!strncmp(*path, syscheck.wdata.device[iterator], dev_size)) {
            size_t new_path_size = strlen(syscheck.wdata.drive[iterator]) + (size_t) (*path - dev_size);

            os_calloc(new_path_size + 1, sizeof(char), new_path);
            snprintf(new_path, new_path_size, "%s%s", syscheck.wdata.drive[iterator], *path + dev_size);
            mdebug2("Replacing '%s' to '%s'", *path, new_path);

            os_free(*path);
            *path = new_path;
            break;
        }

        iterator++;
    }

}

char *get_whodata_path(const short unsigned int *win_path) {
    int count;
    char *path = NULL;

    if (count = WideCharToMultiByte(CP_ACP, 0, win_path, -1, NULL, 0, NULL, NULL), count > 0) {
        os_calloc(count + 1, sizeof(char), path);
        count = WideCharToMultiByte(CP_ACP, 0, win_path, -1, path, count, NULL, NULL);
        path[count] = '\0';
    }

    if (!count) {
        os_free(path);
        mdebug1("The path could not be processed in Whodata mode. Error: %lu.", GetLastError());
    }

    return path;
}

int whodata_path_filter(char **path) {
    if (check_removed_file(*path)) {
        mdebug2("File '%s' is in the recycle bin. It will be discarded.", *path);
        return 1;
    }

    if (sys_64) {
        whodata_adapt_path(path);
    }

    if (OSHash_Get_ex(syscheck.wdata.ignored_paths, *path)) {
        // The file has been marked as ignored
        mdebug2("The file '%s' has been marked as ignored. It will be discarded.", *path);
        return 1;
    }

    return 0;
}

void whodata_adapt_path(char **path) {
    const char *system_32 = ":\\windows\\system32";
    const char *system_wow64 = ":\\windows\\syswow64";
    const char *system_native = ":\\windows\\sysnative";
    char *new_path = NULL;

    if (strstr(*path, system_32)) {
        new_path = wstr_replace(*path, system_32, system_native);

    } else if (strstr(*path, system_wow64)) {
        new_path = wstr_replace(*path, system_wow64, system_32);
    }

    if (new_path) {
        mdebug2("Convert '%s' to '%s' to process the whodata event.", *path, new_path);
        free(*path);
        *path = new_path;
    }
}

int whodata_check_arch() {
    HKEY RegistryKey;
    int retval = OS_INVALID;
    char arch[64 + 1] = "";
    long unsigned int data_size = 64;
    unsigned int result;
    const char *environment_key = "System\\CurrentControlSet\\Control\\Session Manager\\Environment";
    const char *processor_arch = "PROCESSOR_ARCHITECTURE";

    if (RegOpenKeyEx(HKEY_LOCAL_MACHINE, environment_key, 0, KEY_READ, &RegistryKey) != ERROR_SUCCESS) {
        merror(SK_REG_OPEN, environment_key);
        return OS_INVALID;
    } else {
        if (result = RegQueryValueEx(RegistryKey, TEXT(processor_arch), NULL, NULL, (LPBYTE)&arch, &data_size), result != ERROR_SUCCESS) {
            merror("Error reading 'Architecture' from Windows registry. (Error %u)", (unsigned int)result);
        } else {

            if (!strncmp(arch, "AMD64", 5) || !strncmp(arch, "IA64", 4) || !strncmp(arch, "ARM64", 5)) {
                sys_64 = 1;
                retval = 0;
            } else if (!strncmp(arch, "x86", 3)) {
                sys_64 = 0;
                retval = 0;
            }
        }
        RegCloseKey(RegistryKey);
    }

    return retval;
}

int w_update_sacl(const char *obj_path) {
    SYSTEM_AUDIT_ACE *ace = NULL;
    SID_IDENTIFIER_AUTHORITY world_auth = {SECURITY_WORLD_SID_AUTHORITY};
    HANDLE hdle;
    PSECURITY_DESCRIPTOR security_descriptor = NULL;
    PACL old_sacl = NULL;
    PACL new_sacl = NULL;
    long unsigned result;
    unsigned long new_sacl_size;
    int retval = OS_INVALID;
    int privilege_enabled = 0;
    ACL_SIZE_INFORMATION old_sacl_info;
    PVOID entry_access_it = NULL;
    unsigned int i;

    if (!everyone_sid) {
        if (!AllocateAndInitializeSid(&world_auth, 1, SECURITY_WORLD_RID, 0, 0, 0, 0, 0, 0, 0, &everyone_sid)) {
            merror("Could not obtain the sid of Everyone. Error '%lu'.", GetLastError());
            goto end;
        }
    }

    if (!ev_sid_size) {
        ev_sid_size = GetLengthSid(everyone_sid);
    }

    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hdle)) {
        merror("OpenProcessToken() failed. Error '%lu'.", GetLastError());
        goto end;
    }

    if (set_privilege(hdle, priv, TRUE)) {
        merror("The privilege could not be activated. Error: '%ld'.", GetLastError());
        goto end;
    }

    privilege_enabled = 1;

    if (result = GetNamedSecurityInfo(obj_path, SE_FILE_OBJECT, SACL_SECURITY_INFORMATION, NULL, NULL, NULL, &old_sacl, &security_descriptor), result != ERROR_SUCCESS) {
        merror("GetNamedSecurityInfo() failed. Error '%ld'", result);
        goto end;
    }

    ZeroMemory(&old_sacl_info, sizeof(ACL_SIZE_INFORMATION));
    // Get SACL size
    if (old_sacl && !GetAclInformation(old_sacl, (LPVOID)&old_sacl_info, sizeof(ACL_SIZE_INFORMATION), AclSizeInformation)) {
        merror("The size of the '%s' SACL could not be obtained.", obj_path);
        goto end;
    }

    // Set the new ACL size
    new_sacl_size = (old_sacl ? old_sacl_info.AclBytesInUse : sizeof(ACL)) + sizeof(SYSTEM_AUDIT_ACE) + ev_sid_size;

    if (new_sacl = (PACL)win_alloc(new_sacl_size), !new_sacl) {
        merror("No memory could be reserved for the new SACL of '%s'.", obj_path);
        goto end;
    }

    if (!InitializeAcl(new_sacl, new_sacl_size, ACL_REVISION)) {
        merror("The new SACL for '%s' could not be created. Error: '%ld'.", obj_path, GetLastError());
        goto end;
    }

    if (ace = (SYSTEM_AUDIT_ACE *)win_alloc(sizeof(SYSTEM_AUDIT_ACE) + ev_sid_size - sizeof(DWORD)), !ace) {
        merror("No memory could be reserved for the new ACE of '%s'. Error: '%ld'.", obj_path, GetLastError());
        goto end;
    }

    ace->Header.AceType  = SYSTEM_AUDIT_ACE_TYPE;
    ace->Header.AceFlags = FAILED_ACCESS_ACE_FLAG;
    ace->Header.AceSize  = LOWORD(sizeof(SYSTEM_AUDIT_ACE) + ev_sid_size - sizeof(DWORD));
    ace->Mask            = 0;

    if (!CopySid(ev_sid_size, &ace->SidStart, everyone_sid)) {
        merror("Could not copy the everyone SID for '%s'. Error: '%d-%ld'.", obj_path, ev_sid_size, GetLastError());
        goto end;
    }

    if (old_sacl) {
        if (old_sacl_info.AceCount) {
            for (i = 0; i < old_sacl_info.AceCount; i++) {
               if (!GetAce(old_sacl, i, &entry_access_it)) {
                   merror("The ACE number %i for '%s' could not be obtained.", i, obj_path);
                   goto end;
               }

               if (!AddAce(new_sacl, ACL_REVISION, MAXDWORD, entry_access_it, ((PACE_HEADER)entry_access_it)->AceSize)) {
                   merror("The ACE number %i of '%s' could not be copied to the new ACL.", i, obj_path);
                   goto end;
               }
           }
        }
    }

    // Add the new ACE
    if (!AddAce(new_sacl, ACL_REVISION, 0, (LPVOID)ace, ace->Header.AceSize)) {
        merror("The new ACE could not be added to '%s'. Error: '%ld'.", obj_path, GetLastError());
        goto end;
    }

    if (result = SetNamedSecurityInfo((char *) obj_path, SE_FILE_OBJECT, SACL_SECURITY_INFORMATION, NULL, NULL, NULL, new_sacl), result != ERROR_SUCCESS) {
        merror("SetNamedSecurityInfo() failed. Error: '%lu'", result);
        goto end;
    }

    retval = 0;
end:
    if (privilege_enabled && set_privilege(hdle, priv, FALSE)) {
        merror("The privilege could not be activated. Error: '%ld'.", GetLastError());
        goto end;
    }

    if (security_descriptor) {
        LocalFree((HLOCAL)security_descriptor);
    }

    if (old_sacl) {
        LocalFree((HLOCAL)old_sacl);
    }

    if (new_sacl) {
        LocalFree((HLOCAL)new_sacl);
    }

    if (hdle) {
        CloseHandle(hdle);
    }

    if (ace) {
        LocalFree((HLOCAL)ace);
    }

    return retval;
}

void whodata_remove_folder(OSHashNode **row, OSHashNode **node, void *data) {
    whodata_evt *w_dir = (whodata_evt *) data;
    char *dir = w_dir->path;

    if (!strncmp(dir, (*node)->key, strlen(dir))) {
        syscheck_node *s_node = (syscheck_node *) (*node)->data;
        OSHashNode *r_node = *node;
        whodata_evt w_file;
        memcpy(&w_file, w_dir, sizeof(whodata_evt));

        mdebug2("File '%s' was inside the removed directory '%s'. It will be notified.", (*node)->key, dir);

        w_file.scan_directory = 0;
        w_file.path = (*node)->key;
        send_whodata_del(&w_file, 0);

        if ((*node)->prev) {
            (*node)->prev->next = (*node)->next;
        }

        *node = NULL;

        // If the node is the first node of the row
        if (*row == r_node) {
            *row = NULL;
        }

        free(r_node->key);
        free(r_node);
        free(s_node->checksum);
        free(s_node);
    }
}

void whodata_rlist_add(char *id) {
    whodata_event_node *node = NULL;

    os_calloc(sizeof(whodata_event_node), 1, node);

    if (syscheck.w_rlist.last) {
        syscheck.w_rlist.last->next = node;
        node->prev = syscheck.w_rlist.last;
    } else {
        syscheck.w_rlist.first = node;
    }

    os_strdup(id, node->id);
    node->insert_time = time(NULL);
    syscheck.w_rlist.last = node;
}

int whodata_check_removed(char *file) {
    whodata_event_node *node_it;
    time_t now = time(NULL);

    for (node_it = syscheck.w_rlist.last; node_it && node_it->insert_time + syscheck.w_rlist.queue_time >= now; node_it = node_it->prev) {
        if (!strcmp(node_it->id, file)) {
            mdebug2("Ignoring remove event for file '%s' because it has already been reported.", file);
            return 1;
        }
    }

    return 0;
}

void whodata_clean_rlist() {
    whodata_event_node *node_it;
    time_t now = time(NULL);

    for (node_it = syscheck.w_rlist.first; node_it && node_it->insert_time + syscheck.w_rlist.queue_time < now;) {
        whodata_event_node *next = node_it->next;
        whodata_rlist_remove(node_it);
        node_it = next;
        syscheck.w_rlist.first = node_it;
    }
    if (!syscheck.w_rlist.first) {
        syscheck.w_rlist.last = NULL;
    }
}

#endif
