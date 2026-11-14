// Tiny registry so every test class lives in its own file but one binary runs them all.
#pragma once

#include <QObject>
#include <QStringList>
#include <QtTest>
#include <functional>
#include <vector>

struct TestRegistry {
    static std::vector<std::pair<const char *, std::function<int(int, char **)>>> &all() {
        static std::vector<std::pair<const char *, std::function<int(int, char **)>>> list;
        return list;
    }
};

template <typename T> struct TestRegistrar {
    explicit TestRegistrar(const char *name) {
        TestRegistry::all().push_back({name, [](int argc, char **argv) {
            T test;
            return QTest::qExec(&test, argc, argv);
        }});
    }
};

#define REGISTER_TEST(Class) static TestRegistrar<Class> registrar_##Class(#Class);
