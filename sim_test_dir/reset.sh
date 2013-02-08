rm -rf slurm_varios/acct/*
rm -rf slurm_varios/log/*
rm -rf slurm_varios/var/state/*

mysql -u root -p < delete_slurm_tables_info
