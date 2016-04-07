

Vagrant.configure("2") do |config|

  # enable hostmanager
  config.hostmanager.enabled = true
  config.hostmanager.manage_host = true

  #
  # source
  #
  config.vm.define "source" do |node|

    # host settings
    node.vm.hostname = "source"
    node.vm.box = "bento/centos-7.1"
    node.vm.network :private_network, ip: "192.168.33.10", netmask: "255.255.255.0"
    node.ssh.insert_key = "true"

    # provider
    node.vm.provider "virtualbox" do |vb|
      vb.memory = 1024
      vb.cpus = 1
    end
  end

  #
  # sink
  #
  config.vm.define "sink" do |node|

    # host settings
    node.vm.hostname = "sink"
    node.vm.box = "bento/centos-7.1"
    node.vm.network :private_network, ip: "192.168.33.11", netmask: "255.255.255.0"
    node.ssh.insert_key = "true"

    # provider
    node.vm.provider "virtualbox" do |vb|
      vb.memory = 4096
      vb.cpus = 3

      # network adapter settings; [Am79C970A|Am79C973|82540EM|82543GC|82545EM|virtio]
      vb.customize ["modifyvm", :id, "--nicpromisc2", "allow-all"]
      vb.customize ["modifyvm", :id, "--nictype2","82545EM"]
    end
  end

  # provisioning
  config.vm.provision :ansible do |ansible|
    ansible.playbook = "playbook.yml"
  end
end
