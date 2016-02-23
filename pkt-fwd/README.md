Packet Forwarding with DPDK
============================

Uses the Data Plane Development Kit ([DPDK](http://dpdk.org/)) to perform packet forwarding from one network to another.

The provisioning process will create three separate nodes and two separate networks.  The **source** node will transmit network traffic on the 192.168.33.x network.  This traffic is extracted continuously from a libpcap-formatted file.  

The **router** uses a DPDK-based packet forwarder to capture all traffic on 192.168.33.x and forward the packets unmodified to the 192.168.99.x network.  

The **sink** will capture all packets on the 192.168.99.x network.  The **sink** will not be able to capture any network traffic unless the **router** is working properly.

Getting Started
---------------

```
ansible-galaxy install -r requirements.yml
vagrant up
```

*WARNING* If the `testpmd` application fails to start, the number of huge pages may need to be increased to at least 512.

After the provisioning process has completed, run the following commands. Since the router has not been started there should be no packet data captured from this command yet.

```
vagrant ssh sink
tcpdump -i enp0s8
```

Open a second terminal and run the following commands to start the packet forwarder.

```
printf "27 \n" | $RTE_SDK/tools/setup.sh
testpmd> show port info all
testpmd> start
testpmd> show port stats all
```

The first terminal should now display the packet data that is being routed from one network to the next.
