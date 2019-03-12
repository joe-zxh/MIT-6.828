# MIT-6.828(2018)

### 说明
课程首页：https://pdos.csail.mit.edu/6.828/2018/schedule.html

我以读代码为主，参考了大量别人的资料，完成了lab1~lab5。  
lab6 network没有完成，因为lab6涉及网卡硬件的部分比较多，需要读大量有关硬件的文档。


### 参考的资料
lab1~lab6: https://www.jianshu.com/p/782cb80c7690  
lab1～lab3: http://www.cnblogs.com/fatsheep9146/category/769143.html  
lab4: https://www.jianshu.com/p/10f822b3deda?utm_campaign=maleskine&utm_content=note&utm_medium=seo_notes&utm_source=recommendation  
lab5: https://www.e-learn.cn/content/linux/1451584  
https://blog.csdn.net/bysui/article/details/51868917  
lab6: https://blog.csdn.net/bysui/article/details/75088596  
其他相关资料我放到chrome的书签中了：[MIT-6.828资料书签](./docs/MIT-6.828.html)

### 使用方法

1. 安装工具包  
sudo apt-get install -y build-essential libtool libglib2.0-dev libpixman-1-dev zlib1g-dev git libfdt-dev gcc-multilib gdb

2. 创建工作目录  
mkdir MIT-6.828

3. 课程对应的QEMU的代码  
git clone http://web.mit.edu/ccutler/www/qemu.git -b 6.828-2.3.0

4. 进入qemu目录，配置并编译安装  
cd qemu \  
&& ./configure --disable-kvm --disable-sdl --target-list="i386-softmmu x86_64-softmmu" \  
&& make -j8 && make install

5. 获取实验代码  
cd.. && git clone https://github.com/joe-zxh/MIT-6.828.git

6. 运行qemu  
make qemu  或者  
make qemu-nox  (没有gui版本)

7. 运行lab1的评分脚本  
git branch lab1_joe && make grade
