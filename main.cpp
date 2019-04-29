#include <iostream>
#include <sys/stat.h>
#include <unistd.h>
#include <vector>
#include <dirent.h>
#include <cstring>
#include <wait.h>
#include <unordered_map>

using namespace std;

const char fileSeparator =
#ifdef _WIN32
        '\\';
#else
        '/';
#endif

bool file_exists(const string &path) {
    struct stat buffer{};
    return (stat(path.c_str(), &buffer) == 0);
}

struct command {
    std::string path;
    std::vector<char *> argv;

    explicit command(const string &path) : path(path), argv(2) {
        argv[0] = new char[path.size()];
        path.copy(argv[0], path.size());
        argv[1] = nullptr;
    }

    explicit command(const std::vector<std::string> &args) : path(args[0]), argv(args.size() + 1) {
        for (int i = 0; i < args.size(); i++) {
            argv[i] = new char[args[i].size()];
            args[i].copy(argv[i], args[i].size());
        }
        argv.back() = nullptr;
    }

    command() : path(), argv{nullptr} {}

    ~command() {
        for (auto &i : argv) {
            delete[]i;
        }
    }
};

int execve(const command &cmd, const string &path) {
    vector<char *> argv(3);
    argv[0] = const_cast<char *> (cmd.path.data());
    argv[1] = const_cast<char *> (path.data());
    argv[2] = nullptr;
    return execve(cmd.path.data(), argv.data(), environ);
}

struct filter {
private:

    enum COMPARATOR {
        LESS,
        GREATER,
        EQUALS
    };

    vector<ino_t> inodes;
    vector<string> fileNames;

    vector<pair<off_t, COMPARATOR >> sizes;
    vector<nlink_t> hardlinks;
    command exec;

public:
    explicit filter(const vector<pair<string, string>> &modifiers) {
        for (auto &m:modifiers) {
            if (m.first == "-inum") {
                try {
                    inodes.push_back(stoull(m.second));
                } catch (...) {
                    throw runtime_error("invalid inode number: " + m.second);
                }
            } else if (m.first == "-name") {
                fileNames.push_back(m.second);
            } else if (m.first == "-size") {

                try {
                    off_t num = stoull(m.second.substr(1, m.second.size()));
                    if (m.second[0] == '-') {
                        sizes.emplace_back(num, LESS);
                    } else if (m.second[0] == '=') {
                        sizes.emplace_back(num, EQUALS);
                    } else if (m.second[0] == '+') {
                        sizes.emplace_back(num, GREATER);
                    }

                } catch (...) {
                    throw runtime_error("invalid size: " + m.second);
                }
            } else if (m.first == "-nlinks") {
                try {
                    hardlinks.push_back(stoull(m.second));
                } catch (...) {
                    throw runtime_error("invalid hardlinks number: " + m.second);
                }
            } else if (m.first == "-exec") {
                if (!file_exists(m.second)) {
                    throw runtime_error("invalid path to executable: " + m.second);
                }
                exec = command(m.second);
            }
        }
    }

    void invoke(const string &path) const {
        pid_t pid = fork();
        if (pid == -1) {
            throw runtime_error("Can't create fork: " + errno);
        } else if (pid == 0) {
            if (execve(exec, path) == -1) {
                perror("Execution failed");
                exit(EXIT_FAILURE);
            } else {
                exit(EXIT_SUCCESS);
            }
        } else {
            int status;
            if (waitpid(pid, &status, 0) == -1) {
                throw runtime_error("Execution failed:" + errno);
            } else if (WIFSIGNALED(status)) {
                throw runtime_error("Execution is stopped with signal: " + WTERMSIG(status));
            }
        }
    }

    bool apply(struct dirent *file, const string &path) const {
        struct stat st;
        if (stat(path.c_str(), &st) != 0) {
            return false;
        }
        bool tmp = inodes.empty();
        for (auto &inode:inodes) {
            tmp |= (inode == file->d_ino);
        }
        if (!tmp) return false;
        tmp = fileNames.empty();
        for (auto &name:fileNames) {
            tmp |= (name == file->d_name);
        }
        if (!tmp) return false;
        tmp = hardlinks.empty();
        for (auto &hardlink: hardlinks) {
            tmp |= (st.st_nlink != hardlink);
        }
        if (!tmp) return false;
        tmp = sizes.empty();
        for (auto &size_filter:sizes) {
            bool comp = false;
            switch (size_filter.second) {
                case LESS:
                    comp = st.st_size < size_filter.first;
                    break;
                case EQUALS:
                    comp = st.st_size == size_filter.first;
                    break;
                case GREATER:
                    comp = st.st_size > size_filter.first;
                    break;
            }
            tmp |= comp;
        }
        return tmp;
    }

    bool executable() const {
        return !exec.path.empty();
    }
};

void walk(const string &path, const filter &filter) {
    DIR *dir = opendir(path.c_str());
    struct dirent *entry;

    if (dir == nullptr) {
        cout << "Can't access directory " + path << endl;
        return;
    }
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;
        string fullPath = path + fileSeparator + entry->d_name;

        if (entry->d_type == DT_DIR) {
            walk(fullPath, filter);
        } else if (entry->d_type == DT_REG) {
            if (filter.apply(entry, fullPath)) {
                cout << fullPath << endl;
                if (filter.executable()) {
                    filter.invoke(fullPath);
                }
            }
        }
    }

}

void print_usage() {
    cout << "Usage: find PATH [-inum INODE_NUMBER] [-name NAME] [-size [-|=|+]SIZE] [-nlinks LINKS_NUMBER] [-exec PATH]"
         << endl;
};

int main(int argc, char **argv) {
    if (argc <= 1) {
        print_usage();
        return 0;
    }
    string path(argv[1]);
    vector<pair<string, string>> modifiers;
    if (!file_exists(path)) {
        cerr << "File " + path + " isn't accessible." << endl;
        return 0;
    }
    int i = 2;
    while (i < argc) {
        string arg = argv[i];
        i++;
        if (i == argc) {
            cerr << "Incorrect number of argumnets." << endl;
            print_usage();
            return 0;
        }
        if (arg == "-inum" || arg == "-name" || arg == "-nlinks" || arg == "-size" || arg == "-exec") {
            modifiers.emplace_back(arg, argv[i]);
            i++;
        } else {
            cerr << "Invalid argument: " + arg << endl;
            return 0;
        }
    }
    try {
        walk(path, filter(modifiers));
    } catch (runtime_error &e) {
        cerr << e.what() << endl;
    }
    return 0;
}