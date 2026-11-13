package com.jlink.service;

import com.ptc.pfc.pfcAsyncConnection.*;
import com.ptc.pfc.pfcSession.*;
import com.ptc.pfc.pfcModel.*;
import com.ptc.cipjava.*;

/**
 * 编译验证程序：只验证 JDK + otk.jar/pfcasync.jar 能正确编译 J-Link 代码。
 * 不实际连接 Creo（那是 SmokeTest / Server 的事）。
 */
public class CompileCheck {
    public static void main(String[] args) {
        System.out.println("J-Link 类加载验证:");
        // 引用几个关键类，确认 import 与 classpath 正确
        System.out.println("  AsyncConnection    = " + AsyncConnection.class.getName());
        System.out.println("  pfcAsyncConnection = " + pfcAsyncConnection.class.getName());
        System.out.println("  Session            = " + Session.class.getName());
        System.out.println("  Model              = " + Model.class.getName());
        System.out.println("  ModelType.MDL_PART = " + ModelType.MDL_PART);
        System.out.println("编译 + 类路径 OK");
    }
}
