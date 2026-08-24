echo zedBSD-PHASE19-QEMU-START

value=phase19
test "$value" = phase19 && echo zedBSD-PHASE19-SHELL-PASS

echo zedBSD-PHASE19-SERVICE-REQUEST
service list > /tmp/phase19-services
grep -q 'syslogd.*enabled.*running' /tmp/phase19-services && echo zedBSD-PHASE19-SERVICE-LIST-PASS
service status cron | grep -q 'cron enabled running' && echo zedBSD-PHASE19-SERVICE-STATUS-PASS

net show lo0 | grep -q 'lo0 static online' && echo zedBSD-PHASE19-NETWORK-PASS
net down lo0 >/tmp/phase19-net-down && net show lo0 | grep -q 'lo0 static offline' && net up lo0 >/tmp/phase19-net-up && net show lo0 | grep -q 'lo0 static online' && echo zedBSD-PHASE19-NET-CONTROL-PASS

logger -t phase19 zedBSD-PHASE19-LOGGER-RECORD
sleep 2
grep -q zedBSD-PHASE19-LOGGER-RECORD /var/log/messages && echo zedBSD-PHASE19-SYSLOG-PASS

service disable cron >/tmp/phase19-disable
service status cron | grep -q 'cron disabled running' && echo zedBSD-PHASE19-DISABLE-PASS
service enable cron >/tmp/phase19-enable
service status cron | grep -q 'cron enabled running' && echo zedBSD-PHASE19-ENABLE-PASS

echo 'echo zedBSD-PHASE19-AT-JOB-PASS' > /tmp/phase19-at-command
at -f /tmp/phase19-at-command now
sleep 3
grep -q zedBSD-PHASE19-AT-JOB-PASS /var/spool/cron-output/* && echo zedBSD-PHASE19-AT-PASS

echo '* * * * * echo zedBSD-PHASE19-CRON-JOB-PASS' > /tmp/phase19-crontab
crontab /tmp/phase19-crontab
crontab -l | grep -q zedBSD-PHASE19-CRON-JOB-PASS && echo zedBSD-PHASE19-CRONTAB-PASS

echo zedBSD-PHASE19-QEMU-PASS
poweroff
