if BUILD_PHASTHDF

bin_SCRIPTS = phasthdf
CLEANFILES = $(bin_SCRIPTS)

do_subst = sed -e 's,[@]pkglibdir[@],$(pkglibdir),g'

phasthdf: $(srcdir)/phasthdf.in Makefile
	$(do_subst) < $(srcdir)/phasthdf.in > phasthdf
	chmod +x phasthdf

pkglibdatadir = $(pkglibdir)

pkglibdata_DATA = \
	dist64/jhdf5.jar \
	dist64/jhdf5obj.jar \
	dist64/jhdf.jar \
	dist64/jhdfobj.jar \
	dist64/Linux/libjhdf5.so \
	dist64/Linux/libjhdf.so \
	dist64/phast.jar

$(pkglibdata_DATA): dist-Linux64

dist-Linux64:
	$(ANT) -Dsrcdir=@srcdir@ dist-Linux64

# force distclean-generic to be called after clean-local
# for parallel cleans
distclean-generic: clean-local
	-test -z "$(DISTCLEANFILES)" || rm -f $(DISTCLEANFILES)

clean-local:
	$(ANT) -Dsrcdir=@srcdir@ clean

endif

EXTRA_DIST = \
	CMakeLists.txt \
	phasthdf.in \
	dist-lib64/jhdf.jar \
	dist-lib64/jhdf5.jar \
	dist-lib64/jhdf5obj.jar \
	dist-lib64/jhdfobj.jar \
	dist-lib64/phast.jar \
	dist-lib64/Linux/libjhdf.so \
	dist-lib64/Linux/libjhdf5.so \
	dist-lib64/Win32/jhdf.dll \
	dist-lib64/Win32/jhdf5.dll \
	hdf-java/linux/hdf-java-2.3.jar \
	hdf-java/win/hdf-java-2.3.jar \
	hdf-java/solaris/hdf-java-2.3.jar \
	hdf-java/linux64/hdf-java-2.6.1-bin.tar \
	hdf-java/win64/hdf-java-2.6.1-bin.tar \
	src/corejava/Format.java \
	src/gov/usgs/phast/JCheckList.java \
	src/gov/usgs/phast/JMalformedHDFException.java \
	src/gov/usgs/phast/IProgressMonitor.java \
	src/gov/usgs/phast/JPage2Tab.form \
	src/gov/usgs/phast/JPage1.form \
	src/gov/usgs/phast/JPage1.java \
	src/gov/usgs/phast/JPage2.form \
	src/gov/usgs/phast/JPage2.java \
	src/gov/usgs/phast/JWizardFrame.java \
	src/gov/usgs/phast/JPage2Tab.java \
	src/gov/usgs/phast/JPage3.form \
	src/gov/usgs/phast/JPage3.java \
	src/gov/usgs/phast/ModalProgressMonitor.java \
	src/gov/usgs/phast/JWizardPanel.form \
	src/gov/usgs/phast/JWizardPanel.java \
	src/gov/usgs/phast/PhastH5File.java \
	src/gov/usgs/Splash.java \
	src/images/splash.jpeg \
	src/sample/SwingWorker.java \
	test/corejava/FormatTest.java \
	test/corejava/CorejavaSuite.java \
	test/gov/usgs/phast/JPage1Test.java \
	test/gov/usgs/phast/JWizardFrameTest.java \
	test/gov/usgs/phast/JCheckListTest.java \
	test/gov/usgs/phast/JPage2TabTest.java \
	test/gov/usgs/phast/JPage2Test.java \
	test/gov/usgs/phast/JPage3Test.java \
	test/gov/usgs/phast/JPhastHDFTest.java \
	test/gov/usgs/phast/ModalProgressMonitorTest.java \
	test/gov/usgs/phast/JWizardPanelTest.java \
	test/gov/usgs/phast/PhastH5FileTest.java \
	test/gov/usgs/phast/PhastSuite.java \
	test/gov/usgs/UsgsSuite.java \
	test/gov/GovSuite.java \
	win32/CMakeLists.txt \
	win32/phastexport.cpp \
	win32/phastexport.h \
	win32/phastexport.ico \
	win32/phastexport.rc \
	win32/phasthdf_version.h \
	win32/resource.h \
	win32/small.ico \
	win32/StdAfx.cpp \
	win32/StdAfx.h

DISTCLEANFILES = build.xml
