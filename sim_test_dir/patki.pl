#!/usr/bin/perl

$install_dir="/home/tpatki/local/src/slurm-test/sim_test_dir";


print ("Let's check compilation and installation was successful...\n");

if(-e "slurm_programs/sbin/slurmctld"){
    print "I got a slurmctld binary!\n";
}
else{
    die("Something was wrong during slurm building\n");
}

if(-e "slurm_programs/bin/sim_mgr"){
    print "I got a sim_mgr binary!\n";
}
else{
    die("Something was wrong during slurm building\n");
}

print ("It seems fine.\n");


# We need to create a wrapper for executing sbatch preloading simulation library

open OUT,"+>","slurm_programs/bin/sim_sbatch" or die("sim_sbatch creation error");

print OUT "#!/bin/bash\n";

print OUT "PARAMS=`echo \$@`\n";
print OUT "DATE=`date +%s`\n";

print OUT "# Just for debug\n";
print OUT "echo \"LD_LIBRARY_PATH=$install_dir/slurm_varios/lib/slurm LD_PRELOAD=libslurm_sim.so $install_dir/slurm_programs/bin/sbatch\" \$PARAMS > /tmp/sim_sbatch-\$DATE\n";

print OUT "RES=`LD_LIBRARY_PATH=$install_dir/slurm_varios/lib/slurm LD_PRELOAD=libslurm_sim.so $install_dir/slurm_programs/bin/sbatch \$PARAMS > /tmp/sbatch.out`\n";

close OUT;

$chmod_command="chmod 755 $install_dir/slurm_programs/bin/sim_sbatch";
print "Executing $chmod_command\n";
system($chmod_command);

# After slurm has been compiled and installed, let's take the sim_mgr program here

system("objdump -t slurm_programs/sbin/slurmctld | grep _slurmctld_rpc_mgr | awk '{print \$1 \" # _slurmctld_rpc_mgr\" '} > rpc_threads.info");
system("objdump -t slurm_programs/sbin/slurmctld | grep _slurmctld_signal_hand | awk '{print \$1 \" # _slurmctld_signal_hand\" '} >> rpc_threads.info");
system("objdump -t slurm_programs/sbin/slurmctld | grep _agent | awk '{print \$1 \" # _agent\" '} >> rpc_threads.info");


print "Installing configuration files...\n";

$CONFDIR="$SIMDIR/slurm_conf";
chomp($CONFDIR);

#
#  CREATING slurm.conf
#

print "...slurm_conf/slurm.conf";

open OUT,"+>","slurm_conf/slurm.conf" or die("slurm.conf creation error");

print OUT "AuthType=auth/none\n";
print OUT "UsePAM=0\n";
print OUT "SlurmUser=\n";
print OUT "SlurmdUser=\n";
print OUT "ControlMachine=\n";
print OUT "ControlAddr=\n";
print OUT "SlurmctldTimeout=300\n";
print OUT "SlurmdTimeout=300\n";
print OUT "MessageTimeout=60\n";
print OUT "ReturnToService=1\n";
print OUT "CryptoType=crypto/openssl\n";
print OUT "JobCredentialPrivateKey=$CONFDIR/slurm.key\n";
print OUT "JobCredentialPublicCertificate=$CONFDIR/slurm.cert\n";
print OUT "PluginDir=$SIMDIR/slurm_varios/lib/slurm\n";
print OUT "TaskPlugin=task/none\n";
print OUT "PropagatePrioProcess=0\n";
print OUT "PropagateResourceLimitsExcept=CPU\n";
print OUT "ProctrackType=proctrack/linuxproc\n";
print OUT "KillWait=60\n";
print OUT "WaitTime=120\n";
print OUT "MaxJobCount=8000\n";
print OUT "MinJobAge=300\n";
print OUT "OverTimeLimit=1\n";
print OUT "JobAcctGatherType=jobacct_gather/linux\n";
print OUT "JobCompType=jobcomp/filetxt\n";
print OUT "JobCompLoc=$SIMDIR/slurm_varios/acct/jobcomp.log\n";
print OUT "SlurmctldDebug=9\n";
print OUT "SlurmctldLogFile=$SIMDIR/slurm_varios/log/slurmctld.log\n";
print OUT "SlurmdDebug=9\n";
print OUT "SlurmdLogFile=$SIMDIR/slurm_varios/log/slurmd.log\n";
print OUT "SlurmdSpoolDir=$SIMDIR/slurm_varios/var/spool\n";
print OUT "StateSaveLocation=$SIMDIR/slurm_varios/var/state\n";
print OUT "CacheGroups=0\n";
print OUT "FastSchedule=1\n";
print OUT "CheckpointType=checkpoint/none\n";
print OUT "SwitchType=switch/none\n";
print OUT "FirstJobId=1000\n";
print OUT "SchedulerType=sched/backfill\n";
print OUT "SchedulerParameters=bf_interval=300,max_job_bf=10\n";
print OUT "PriorityDecayHalfLife=7-0\n";
print OUT "SelectType=select/cons_res\n";
print OUT "SelectTypeParameters=CR_CPU\n";
print OUT "PriorityType=priority/multifactor\n";
print OUT "PriorityWeightAge=100\n";
print OUT "PriorityWeightFairShare=10000\n";
print OUT "PriorityWeightQOS=1000000000\n";
print OUT "PriorityWeightPartition=0\n";
print OUT "PriorityWeightJobSize=0\n";
print OUT "AccountingStorageType=accounting_storage/slurmdbd\n";
print OUT "AccountingStorageHost=localhost\n";
print OUT "AccountingStorageEnforce=limits,qos\n";
print OUT "ClusterName=\n";
print OUT "include $SIMDIR/slurm_conf/slurm.nodes\n";
print OUT "PartitionName=projects Nodes=n[1-122] Default=YES MaxTime=INFINITE State=UP\n";
print OUT "PartitionName=debug Nodes=n[123-126] Default=NO MaxTime=20:00 State=UP\n";
print OUT "FrontendName=\n";


close OUT;

print "\nslurm.conf created\n";
print "\There are some parameters there to adjust before slurm can run:\n";
print "\n\t\tSlurmUser\n";
print "\n\t\tSlurmdUser\n";
print "\n\t\tControlMachine\n";
print "\n\t\tControlAddr\n";
print "\n\t\tClusterName\n";
print "\n\t\tFrontendName\n";
print "\n\nDO NOT FORGET IT!\n\n";


#
#  CREATING slurm.nodes
#

print "...slurm_conf/slurm.nodes";

open OUT,"+>","slurm_conf/slurm.nodes" or die("slurm.nodes creation error");

print OUT "NodeName=DEFAULT RealMemory=3168 Procs=12 Sockets=2 CoresPerSocket=6 ThreadsPerCore=1\n";
print OUT "NodeName=n[1-126] NodeAddr=bscop134 NodeHostName=bscop134 Procs=12 Sockets=2 CoresPerSocket=6 ThreadsPerCore=1\n";

close OUT;

#
#  CREATING slurmdbd.conf
#

print "...slurm_conf/slurmdbd.conf\n";

open OUT,"+>", "slurm_conf/slurmdbd.conf" or die("slurmdbd.conf creation error");

print OUT "DbdHost=localhost\n";
print OUT "AuthType=auth/none\n";
print OUT "LogFile=$SIMDIR/slurm_varios/log/slurmdbd.log\n";
print OUT "PluginDir=$SIMDIR/slurm_varios/lib/slurm\n";
print OUT "StorageHost=localhost\n";
print OUT "StorageType=accounting_storage/mysql\n";
print OUT "StorageUser=\n";
print OUT "StoragePass=\n";
print OUT "SlurmUser=\n";
print OUT "PrivateData=accounts,jobs,reservations,usage,users\n";


print "\nslurmdbd.conf created\n";
print "\There are some parameters there to adjust before slurmdbd can run:\n";
print "\n\t\tStorageUser\n";
print "\n\t\tStoragePass\n";
print "\n\t\tSlurmUser\n";
print "\n\nDO NOT FORGET IT!\n\n";


