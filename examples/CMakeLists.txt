
install(FILES README
	      prun4lsblk.sh
	      prun4lscpu.sh
	      prun4lsmem.sh
	      prun4lsscsi.sh
	      prun4lsscsi_nvme.sh
	      prun4lsucpd.sh
	      test_all.sh 	DESTINATION "${CMAKE_INSTALL_DOCDIR}/examples")
