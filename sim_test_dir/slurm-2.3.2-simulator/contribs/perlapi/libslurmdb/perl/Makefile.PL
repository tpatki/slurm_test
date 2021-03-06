use 5.008;
use ExtUtils::MakeMaker;

if (!(-e "/home/tpatki/local/src/slurm-test/sim_test_dir/slurm_programs/lib/libslurmdb.so") && !(-e "../../../../src/db_api/.libs/libslurmdb.so")) {
	die("I can't seem to find the library files I need in your SLURM installation.
Please check that you have SLURM installation has at least one of the
following link(s):

   ../../../../src/db_api/.libs/libslurmdb.so
   /home/tpatki/local/src/slurm-test/sim_test_dir/slurm_programs/lib/libslurmdb.so\n");
}

# Most all the extra code is to deal with MakeMaker < 6.11 not working
# correctly to build rpms

my(
   $mm_version,
   $mm_knows_destdir,
   $mm_has_destdir,
   $mm_has_good_destdir,
   $mm_needs_destdir,
   );

# Gather some information about what EU::MM offers and/or needs

# Store the version for later use
$mm_version          = $ExtUtils::MakeMaker::VERSION;

# MakeMaker prior to 6.11 doesn't support DESTDIR which is needed for
# packaging with builddir!=destdir. See bug 2388.
$mm_knows_destdir    = $ExtUtils::MakeMaker::Recognized_Att_Keys{DESTDIR};
$mm_has_good_destdir = $mm_version >= 6.11;
# Add DESTDIR hack only if it's requested (and necessary)
$mm_needs_destdir    = !$mm_has_good_destdir;
$mm_has_destdir      = $mm_knows_destdir || $mm_needs_destdir;
$ExtUtils::MakeMaker::Recognized_Att_Keys{"DESTDIR"} = 1 if $mm_needs_destdir;

if ($mm_needs_destdir) {
	my $error = <<DESTDIR_HACK;

    ***********************************************************************
    ExtUtils::MakeMaker ${mm_version} does not include support for DESTDIR,
    so if you want to be on the safe side, you might want to upgrade your
    ExtUtils::MakeMaker to version 6.11 or later. It is available via CPAN.

    You can use either the CPAN shell or go to
      <http://search.cpan.org/search?module=ExtUtils::MakeMaker>
    to get an up-to-date version.

    This should only be necessary if you are creating binary packages.
    ***********************************************************************

DESTDIR_HACK
       $error =~ s/^ {4}//gm;
       warn $error;
} elsif (!$mm_has_good_destdir) {
	my $error = <<DESTDIR_BUG;

    ***********************************************************************
    ExtUtils::MakeMaker ${mm_version} contains bugs that may cause problems
    in the \"make\" process.  It is recommended that you upgrade
    ExtUtils::MakeMaker to version 6.11 or later. It is available via CPAN.

    You can use either the CPAN shell or go to
      <http://search.cpan.org/search?module=ExtUtils::MakeMaker>
    to get an up-to-date version.

    This should only be necessary if you are creating binary packages.
    ***********************************************************************

DESTDIR_BUG
       $error =~ s/^ {4}//gm;
       warn $error;
}

# AIX has problems with not always having the correct
# flags so we have to add some :)
my $os = lc(`uname`);
my $other_ld_flags = "-Wl,-rpath,../../../../src/db_api/.libs -Wl,-rpath,/home/tpatki/local/src/slurm-test/sim_test_dir/slurm_programs/lib";
$other_ld_flags = " -brtl -G -bnoentry -bgcbypass:1000 -bexpfull"
	if $os =~ "aix";

WriteMakefile(
    NAME              => 'Slurmdb',
    VERSION_FROM      => 'Slurmdb.pm', # finds $VERSION
    PREREQ_PM         => {}, # e.g., Module::Name => 1.1
    ($] >= 5.005 ?     ## Add these new keywords supported since 5.005
      (ABSTRACT_FROM  => 'Slurmdb.pm', # retrieve abstract from module
       AUTHOR         => 'Don Lipari <lipari@llnl.gov>') : ()),
    LIBS              => ["-L../../../../src/db_api/.libs -L/home/tpatki/local/src/slurm-test/sim_test_dir/slurm_programs/lib -lslurmdb"], # e.g., '-lm'
    DEFINE            => '', # e.g., '-DHAVE_SOMETHING'
    INC               => "-I. -I../../../.. -I../../../../contribs/perlapi/common -I../../../..",
    # Un-comment this if you add C files to link with later:
    OBJECT            => '$(O_FILES)', # link all the C files too
    CCFLAGS           => '-g',
    PM                => {'Slurmdb.pm' => '$(INST_LIBDIR)/Slurmdb.pm'},
    dynamic_lib       => {'OTHERLDFLAGS' => $other_ld_flags},
);
if  (eval {require ExtUtils::Constant; 1}) {
  # If you edit these definitions to change the constants used by this module,
  # you will need to use the generated const-c.inc and const-xs.inc
  # files to replace their "fallback" counterparts before distributing your
  # changes.
  my @names = (qw(SLURMDB_CLASSIFIED_FLAG
		 SLURMDB_CLASS_BASE SLURMDB_PURGE_ARCHIVE SLURMDB_PURGE_BASE
		 SLURMDB_PURGE_DAYS SLURMDB_PURGE_FLAGS SLURMDB_PURGE_HOURS
		 SLURMDB_PURGE_MONTHS),
              );
  ExtUtils::Constant::WriteConstants(
                                     NAME         => 'Slurmdb',
                                     NAMES        => \@names,
                                     C_FILE       => 'const-c.inc',
                                     XS_FILE      => 'const-xs.inc',
                                  );

}
else {
	die "Error creating constant files: $!";
}
