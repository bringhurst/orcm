/*
 * BEWARE BEWARE BEWARE BEWARE BEWARE BEWARE BEWARE BEWARE BEWARE
 * This file has been auto-generated by the confdc compiler.
 * Source: orcm.fxs
 * BEWARE BEWARE BEWARE BEWARE BEWARE BEWARE BEWARE BEWARE BEWARE
 */

#ifndef _ORCM_YANG_H_
#define _ORCM_YANG_H_
     
#ifdef __cplusplus
extern "C" {
#endif /* __cplusplus */

#include <confd.h>

#ifndef orcm__init_defined
#define orcm__init_defined
extern void orcm__init(void);
#endif

#ifndef orcm__ns
#define orcm__ns 244779220
#define orcm__ns_id "http://www.cisco.com/ns/orcm"
#define orcm__ns_uri "http://www.cisco.com/ns/orcm"
#endif

#define orcm_ORTE_JOB_STATE_ABORTED_WO_SYNC 10
#define orcm_ORTE_JOB_STATE_INIT 1
#define orcm_LCs 2
#define orcm_ORTE_JOB_STATE_FAILED_TO_START 8
#define orcm_ORTE_JOB_STATE_ABORTED 7
#define orcm_ORTE_PROC_STATE_TERMINATED 6
#define orcm_ORTE_PROC_STATE_LAUNCHED 3
#define orcm_ORTE_JOB_STATE_RUNNING 4
#define orcm_ORTE_JOB_STATE_UNDEF 0
#define orcm_ORTE_JOB_STATE_ABORTED_BY_SIG 9
#define orcm_RPs 1
#define orcm_ORTE_PROC_STATE_ABORTED_WO_SYNC 10
#define orcm_not_SPs 3
#define orcm_ORTE_PROC_STATE_ABORTED_BY_SIG 9
#define orcm_ORTE_JOB_STATE_ABORT_ORDERED 13
#define orcm_ORTE_JOB_STATE_NEVER_LAUNCHED 12
#define orcm_ORTE_PROC_STATE_FAILED_TO_START 8
#define orcm_ORTE_JOB_STATE_UNTERMINATED 5
#define orcm_ORTE_PROC_STATE_INIT 1
#define orcm_ORTE_JOB_STATE_RESTART 2
#define orcm_ORTE_PROC_STATE_RESTART 2
#define orcm_ORTE_PROC_STATE_KILLED_BY_CMD 11
#define orcm_ORTE_PROC_STATE_UNDEF 0
#define orcm_ORTE_JOB_STATE_KILLED_BY_CMD 11
#define orcm_ORTE_PROC_STATE_UNTERMINATED 5
#define orcm_all 0
#define orcm_ORTE_PROC_STATE_RUNNING 4
#define orcm_ORTE_JOB_STATE_TERMINATED 6
#define orcm_ORTE_PROC_STATE_ABORTED 7
#define orcm_ORTE_JOB_STATE_LAUNCHED 3
#define orcm_loc 2031061964
#define orcm_max_global_restarts 910723330
#define orcm_replica 861726632
#define orcm_sensor_data 1175993476
#define orcm_app_name 1425497104
#define orcm_replicas 423945804
#define orcm_job_state 707599105
#define orcm_rml_contact_info 835057509
#define orcm_run 288610398
#define orcm_install 1285442628
#define orcm_state 630973766
#define orcm_path 1002915403
#define orcm_num_replicas_terminated 1471587716
#define orcm_hearbeat_seconds 1244091022
#define orcm_daemon_id 115248789
#define orcm_node_id 98319925
#define orcm_app_context_id 1986328700
#define orcm_local_max_restarts 2042655077
#define orcm_exec 1716494663
#define orcm_instance_name 965600200
#define orcm_global_max_restarts 1621547032
#define orcm_replica_name 996821894
#define orcm_job_id 1787667438
#define orcm_leader_exclude 739990340
#define orcm_config 2105663071
#define orcm_exec_name 1057029957
#define orcm_num_replicas_launched 362091585
#define orcm_app_context 540634402
#define orcm_num_procs 1596763349
#define orcm_version 1714291735
#define orcm_orte_node 1848244854
#define orcm_class 284378921
#define orcm_exit_code 1333243938
#define orcm_location 1827424758
#define orcm_config_set 1930476976
#define orcm_node 2084421367
#define orcm_gid 1021704153
#define orcm_job_name 1228169600
#define orcm_names 790727664
#define orcm_argv 1848202598
#define orcm_temperature 362670904
#define orcm_env 2041620060
#define orcm_default 515137925
#define orcm_nodes 650955465
#define orcm_aborted 1997054869
#define orcm_pid 397036642
#define orcm_app_instance 172109319
#define orcm_app 1820663438
#define orcm_appl_context_id 162007192
#define orcm_max_local_restarts 651287464
#define orcm_num_replicas_requested 1177187276
#define orcm_name 1998270519
#define orcm_job 1295289048
#define orcm_uid 1587813883
#define orcm_aborted_procedure 1958506981
#define orcm_replica_state 758812399
#define orcm_oper 1313484953
#define orcm_working_dir 1669518446
#define orcm__callpointid_orcm_oper "orcm_oper"
#define orcm__callpointid_orte_node "orte-node"

#ifdef __cplusplus
}
#endif

#endif
