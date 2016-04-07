Paccelerate
===========

A [Metron](https://metron.incubator.apache.org/) probe that performs network packet capture leveraging the Data Plane Development Kit ([DPDK](http://dpdk.org/)).

Getting Started
---------------

```
vagrant plugin install vagrant-hostmanager
vagrant plugin install vagrant-vbguest
ansible-galaxy install -r requirements.yml
vagrant up
```

To manually run the capture process, run the following command.

```
pcapture -- -p 0x1 -t pcap -c /etc/paccelerate.conf
```
