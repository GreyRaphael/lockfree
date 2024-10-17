#pragma once

struct BarData {
    int id;
    char symbol[7];
    double price;
    long volume;
    double amount;
};

struct TickData {
    int id;
    char symbol[7];
    double open;
    double high;
    int volumes[10];
};
