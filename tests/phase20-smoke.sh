echo zedBSD-PHASE20-QEMU-START

service status networkd | grep -q 'networkd enabled running' && echo zedBSD-PHASE20-NETWORKD-READY-PASS
service status net | grep -q 'net enabled completed' && echo zedBSD-PHASE20-NET-SERVICE-PASS

ifconfig lo0 | grep -q 'inet 127.0.0.1 netmask 255.0.0.0' && echo zedBSD-PHASE20-LOOPBACK-PASS
ifconfig ne0 | grep -q 'inet 10.0.2.' && echo zedBSD-PHASE20-DHCP-ADDRESS-PASS
route -n show | grep -q 'default.*10.0.2.2' && echo zedBSD-PHASE20-DHCP-ROUTE-PASS
grep -q 'nameserver 10.0.2.3' /etc/resolv.conf && test "$(grep -c 'nameserver 10.0.2.3' /etc/resolv.conf)" = 1 && echo zedBSD-PHASE20-DNS-PASS

test -x /sbin/dhcpc && test ! -e /sbin/dhcpcd && echo zedBSD-PHASE20-DHCPC-IDENTITY-PASS
ps | grep -q '[d]hcpc' || echo zedBSD-PHASE20-DHCPC-ONESHOT-PASS

net down lo0 && net show lo0 | grep -q 'lo0 static offline' && net up lo0 && net show lo0 | grep -q 'lo0 static online' && echo zedBSD-PHASE20-NET-CONTROL-PASS

service stop networkd
ifconfig lo0 down && ifconfig lo0 up && echo zedBSD-PHASE20-DIRECT-IFCONFIG-PASS
service start networkd
echo zedBSD-PHASE20-RESTART-STATUS-BEGIN
service status networkd | grep -q 'networkd enabled running'
echo zedBSD-PHASE20-RESTART-NET-BEGIN
net show lo0 | grep -q 'lo0 static online' && echo zedBSD-PHASE20-RESTART-PASS

echo zedBSD-PHASE20-QEMU-PASS
poweroff
