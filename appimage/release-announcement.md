% Continuous build (AppImage)
% Kurt Pfeifle
%


# First *`ippsample`* AppImage

This is my first shot at building an AppImage for the *Sample IPP (Internet Printing Protocol) Server* with *assorted helper utilities*. The code is provided by the [ISTO Printer Working Group (PWG)](https://www.pwg.org), which I forked for this building project. Hopefully, one day soon I can create an acceptable pull request for [upstream](https://github.com/istopwg/ippsample).

Since this AppImage deviates a bit from the standard AppImage philosophy (*"One App == One File"*) as it provides *multiple apps inside one AppImage file*, you should prepare your environment for conveniently accessing all IPP testing utilities embedded in the AppImage:

     mkdir $HOME/AppImages
     cd $HOME/AppImages
     wget $URL-of-AppImage
     mkdir $HOME/bin
     cd $HOME/bin
     ln -s ../AppImages/ippsample-x86_64.AppImage ippserver
     ln -s ../AppImages/ippsample-x86_64.AppImage ippfind
     ln -s ../AppImages/ippsample-x86_64.AppImage ipptool
     ln -s ../AppImages/ippsample-x86_64.AppImage ippproxy
     export PATH=$HOME/bin:$PATH

The tools in this package serve for testing the IPP capabilities of modern printers. They can also provide a practical learning platform for all things IPP. All these utilities are meant to be used from the command line:

    ippserver
    ippfind
    ipptool
    ippproxy

(The additional *`ipptransform`* and *`ipptransform3d`* tools from the upstream IPP Sample code are not part of the AppImage at this time, sorry.)

The AppImage comes with a custom AppRun providing additional help. Try to run it with *`ippsample-*.AppImage help`* first. Then walk from there, and try:

    ippsample-x86_64.AppImage listman
    ippsample-x86_64.AppImage man 1 ipptool
    ippsample-x86_64.AppImage listhtml
    ippsample-x86_64.AppImage html api.html

In case you followed the advice above to create symlinks named *`ipp{server,find,tool,proxy}`* pointing to the real AppImage, then of course you can also run (instead any of the above):

    ippserver listman
    ippserver man 1 ipptool
    ippfind listhtml
    ipptool html api.html

