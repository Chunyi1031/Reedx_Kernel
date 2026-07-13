#include <iostream>
#include <fstream>
#include <string>
#include <cstdlib>
#include <filesystem>
#include <algorithm>

namespace fs = std::filesystem;

std::string readLine(const std::string& filename, int lineNum) {
    std::ifstream file(filename);
    std::string line;
    for (int i = 0; i < lineNum; ++i) {
        if (!std::getline(file, line)) {
            return "";//文件行数不够
        }
    }
    return line;// 已自动去掉换行符
}

std::string replaceSlash(const std::string& input) {
    std::string result = input;
    std::replace(result.begin(), result.end(), '/', '.');
    return result;
}

int main(){
    if (!fs::exists("build")) fs::create_directory("build");
    std::string CC_OPT = readLine("options.txt", 1);//编译参数
    std::string LD_OPT = readLine("options.txt", 2);//链接参数
    if(CC_OPT.empty() || LD_OPT.empty()){
        std::cerr << "Error: options.txt is empty or missing." << std::endl;
        return 1;
    }
    std::string module;
    int module_count = 1;
    while(1){
        module = readLine("modules.txt", module_count);
        if(module.empty() || (module == "END") || (module.substr(0,1) == "#"))break;//读取到空行或END则结束
        int file_count = 1;
        //编译该模块各个源文件
        while(1){
            std::string file = readLine(module+"/modules.txt", file_count);
            if(file.empty() || (file == "END") || (file.substr(0,1) == "#"))break;//读取到空行或END则结束
            std::string cc_command = "x86_64-linux-gnu-gcc " + CC_OPT + " " + module + "/" + file + " -o build/" + module + "_" + replaceSlash(file) + ".o";
            std::cout << "CC     " <<  module + "/" + file << " -> build/" << module + "_" + replaceSlash(file) + ".o" << std::endl;
            int res = std::system(cc_command.c_str());
            if(res != 0){
                std::cerr << "Error:编译文件\"" << module + "/" + file << "\"时失败，编译结束 :(\n";
                return res;
            }
            file_count++;
        }
        //链接该模块的目标文件为单目标文件
        std::string ld_module_command = "x86_64-linux-gnu-ld -r build/" + module + "_*.o -o build/MOD_" + module + ".o ";
        std::cout << "LD [M] " << module << std::endl;
        int res = std::system(ld_module_command.c_str());
        if(res != 0){
            std::cerr << "Error:链接模块\"" << module << "\"时失败，链接结束 :(\n";
            return res;
        }
        module_count++;
    }
    //链接所有模块为内核
    std::string ld_kernel_command = "x86_64-linux-gnu-ld " + LD_OPT + " build/MOD_*.o -o kernel.elf";
    std::cout << "LD [K] kernel.elf" << std::endl;
    int res = std::system(ld_kernel_command.c_str());
    if(res != 0){
        std::cerr << "Error:链接内核时失败，链接结束 :(\n";
        return res;
    }
    std::cout << "编译完成 :)" << std::endl;
    return 0;
}
