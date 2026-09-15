/* SPDX-License-Identifier: Apache-2.0
 *
 * 安眠科技 · PC 进程入口（只在模拟器链里编译）
 *
 * 为什么单独开一个文件而不是在 mianyu_app_main.c 里 #ifdef：
 *   真机上应用入口叫 mianyu_main(argc, argv)，openvela 的 nuttx_add_application
 *   会在链接期自动把 "main" 别名到 "<appname>_main"（见 app/mianyu/CMakeLists.txt
 *   里的注释）。也就是说【真机侧根本不需要我们写 main】。
 *   PC 上没有这套应用框架，必须有人提供一个 main 才链得上（否则报
 *   undefined reference to `WinMain' 之类）。
 *
 * 把这段平台胶水放在 hal/sim/ 下，好处是：
 *   1) mianyu_app_main.c 保持平台无关，一个字都不用改（双 HAL 的约定）；
 *   2) hal/sim/ 本来就只在 PC 链里编译（真机的 CMakeLists 不含它），
 *      所以这段 main 不可能污染真机固件。
 *
 * 两个后端的符号对应关系：
 *   真机：nuttx_add_application 生成 main  →  mianyu_main()
 *   PC  ：本文件 main()                    →  mianyu_main()
 */
extern int mianyu_main(int argc, char *argv[]);

int main(void)
{
    return mianyu_main(0, 0);
}
