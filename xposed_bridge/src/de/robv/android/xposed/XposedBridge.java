package de.robv.android.xposed;

import static de.robv.android.xposed.XposedHelpers.findAndHookMethod;
import static de.robv.android.xposed.XposedHelpers.getBooleanField;
import static de.robv.android.xposed.XposedHelpers.getIntField;
import static de.robv.android.xposed.XposedHelpers.getObjectField;
import static de.robv.android.xposed.XposedHelpers.setObjectField;
import static de.robv.android.xposed.XposedHelpers.setStaticObjectField;

import java.io.BufferedReader;
import java.io.File;
import java.io.FileInputStream;
import java.io.FileReader;
import java.io.FileWriter;
import java.io.IOException;
import java.io.InputStream;
import java.io.InputStreamReader;
import java.io.PrintWriter;
import java.lang.ref.WeakReference;
import java.lang.reflect.AccessibleObject;
import java.lang.reflect.Constructor;
import java.lang.reflect.InvocationTargetException;
import java.lang.reflect.Member;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.text.DateFormat;
import java.util.Arrays;
import java.util.Date;
import java.util.HashMap;
import java.util.HashSet;
import java.util.Map;
import java.util.Set;
import java.util.zip.ZipEntry;
import java.util.zip.ZipInputStream;

import android.annotation.SuppressLint;
import android.app.ActivityThread;
import android.app.AndroidAppHelper;
import android.app.LoadedApk;
import android.content.ComponentName;
import android.content.pm.ApplicationInfo;
import android.content.res.CompatibilityInfo;
import android.content.res.Resources;
import android.content.res.TypedArray;
import android.content.res.XResources;
import android.content.res.XResources.XTypedArray;
import android.os.Build;
import android.os.Process;
import android.util.Log;

import com.android.internal.os.RuntimeInit;
import com.android.internal.os.ZygoteInit;

import dalvik.system.PathClassLoader;
import de.robv.android.xposed.XC_MethodHook.MethodHookParam;
import de.robv.android.xposed.callbacks.XC_InitPackageResources;
import de.robv.android.xposed.callbacks.XC_InitPackageResources.InitPackageResourcesParam;
import de.robv.android.xposed.callbacks.XC_LoadPackage;
import de.robv.android.xposed.callbacks.XC_LoadPackage.LoadPackageParam;
import de.robv.android.xposed.callbacks.XCallback;

public final class XposedBridge {
    public static final String INSTALLER_PACKAGE_NAME = "de.robv.android.xposed.installer";
    public static int XPOSED_BRIDGE_VERSION;

    private static PrintWriter logWriter = null;
    // log for initialization of a few mods is about 500 bytes, so 2*20 kB (2*~350 lines) should be enough
    public static boolean disableResources = false;

    private static final Object[] EMPTY_ARRAY = new Object[0];
    public static final ClassLoader BOOTCLASSLOADER = ClassLoader.getSystemClassLoader();
    @SuppressLint("SdCardPath")
    public static final String BASE_DIR = "/system/xposed/";

    // built-in handlers
    private static final Map<Member, CopyOnWriteSortedSet<XC_MethodHook>> sHookedMethodCallbacks
    = new HashMap<Member, CopyOnWriteSortedSet<XC_MethodHook>>();
    private static final CopyOnWriteSortedSet<XC_LoadPackage> sLoadedPackageCallbacks
    = new CopyOnWriteSortedSet<XC_LoadPackage>();
    private static final CopyOnWriteSortedSet<XC_InitPackageResources> sInitPackageResourcesCallbacks
    = new CopyOnWriteSortedSet<XC_InitPackageResources>();

    /**
     * Called when native methods and other things are initialized, but before preloading classes etc.
     */
    private static void main(String[] args) {
        // the class the VM has been created for or null for the Zygote process
        String startClassName = getStartClassName();

        try {
            log("Loading Xposed (for " + (startClassName == null ? "Zygote" : startClassName) + ")...");
            if (startClassName == null) {
                // Zygote
                log("Running ROM '" + Build.DISPLAY + "' with fingerprint '" + Build.FINGERPRINT + "'");
            }

            if (initNative()) {
                if (startClassName == null) {
                    // Initializations for Zygote
                    initXbridgeZygote();
                }

                loadModules(startClassName);
            } else {
                log("Errors during native Xposed initialization");
            }
        } catch (Throwable t) {
            log("Errors during Xposed initialization");
            log(t);
        }

        // call the original startup code
        if (startClassName == null) {
            log("xposed: calling ZygoteInit");
            ZygoteInit.main(args);
        } else {
            log("xposed: calling RuntimeInit");
            RuntimeInit.main(args);
        }
    }

    private static native String getStartClassName();

    private static int extractIntPart(String str) {
        int result = 0, length = str.length();
        for (int offset = 0; offset < length; offset++) {
            char c = str.charAt(offset);
            if ('0' <= c && c <= '9')
                result = result * 10 + (c - '0');
            else
                break;
        }
        return result;
    }

    /**
     * Hook some methods which we want to create an easier interface for developers.
     */
    private static void initXbridgeZygote() throws Throwable {
        final HashSet<String> loadedPackagesInProcess = new HashSet<String>(1);

        // normal process initialization (for new Activity, Service,
        // BroadcastReceiver etc.)
        findAndHookMethod(ActivityThread.class, "hello", Integer.TYPE, Integer.TYPE, Integer.TYPE, Integer.TYPE, new XC_MethodHook() {
                protected void beforeHookedMethod(MethodHookParam param) throws Throwable {
                    Log.e("xposed_bridge", "beforeHookedMethod, do nothing");
                }
            });
        findAndHookMethod(ActivityThread.class, "hello2", Integer.TYPE, Integer.TYPE, new XC_MethodHook() {
                protected void beforeHookedMethod(MethodHookParam param) throws Throwable {
                    Log.e("xposed_bridge", "beforeHookedMethod 2, do nothing");
                }
            });        
        // findAndHookMethod(ActivityThread.class, "handleBindApplication", "android.app.ActivityThread.AppBindData", new XC_MethodHook() {
        //         protected void beforeHookedMethod(MethodHookParam param) throws Throwable {
        //             ActivityThread activityThread = (ActivityThread) param.thisObject;
        //             ApplicationInfo appInfo = (ApplicationInfo) getObjectField(param.args[0], "appInfo");
        //             ComponentName instrumentationName = (ComponentName) getObjectField(param.args[0], "instrumentationName");
        //             if (instrumentationName != null) {
        //                 XposedBridge.log("Instrumentation detected, disabling framework for " + appInfo.packageName);
        //                 disableHooks = true;
        //                 return;
        //             }
        //             CompatibilityInfo compatInfo = (CompatibilityInfo) getObjectField(param.args[0], "compatInfo");
        //             if (appInfo.sourceDir == null)
        //                 return;

        //             setObjectField(activityThread, "mBoundApplication", param.args[0]);
        //             loadedPackagesInProcess.add(appInfo.packageName);
        //             LoadedApk loadedApk = activityThread.getPackageInfoNoCheck(appInfo, compatInfo);
        //             // XResources.setPackageNameForResDir(appInfo.packageName, loadedApk.getResDir());

        //             LoadPackageParam lpparam = new LoadPackageParam(sLoadedPackageCallbacks);
        //             lpparam.packageName = appInfo.packageName;
        //             lpparam.processName = (String) getObjectField(param.args[0], "processName");
        //             lpparam.classLoader = loadedApk.getClassLoader();
        //             lpparam.appInfo = appInfo;
        //             lpparam.isFirstApplication = true;
        //             XC_LoadPackage.callAll(lpparam);

        //             if (appInfo.packageName.equals(INSTALLER_PACKAGE_NAME))
        //                 hookXposedInstaller(lpparam.classLoader);
        //         }
        //     });

        // system thread initialization
        // findAndHookMethod("com.android.server.ServerThread", null,
        //                   Build.VERSION.SDK_INT < 19 ? "run" : "initAndLoop", new XC_MethodHook() {
        //                           @Override
        //                           protected void beforeHookedMethod(MethodHookParam param) throws Throwable {
        //                               loadedPackagesInProcess.add("android");

        //                               LoadPackageParam lpparam = new LoadPackageParam(sLoadedPackageCallbacks);
        //                               lpparam.packageName = "android";
        //                               lpparam.processName = "android"; // it's actually system_server, but other functions return this as well
        //                               lpparam.classLoader = BOOTCLASSLOADER;
        //                               lpparam.appInfo = null;
        //                               lpparam.isFirstApplication = true;
        //                               XC_LoadPackage.callAll(lpparam);
        //                           }
        //                       });

        // when a package is loaded for an existing process, trigger the callbacks as well
        // hookAllConstructors(LoadedApk.class, new XC_MethodHook() {
        //     @Override
        //     protected void afterHookedMethod(MethodHookParam param) throws Throwable {
        //         LoadedApk loadedApk = (LoadedApk) param.thisObject;

        //         String packageName = loadedApk.getPackageName();
        //         XResources.setPackageNameForResDir(packageName, loadedApk.getResDir());
        //         if (packageName.equals("android") || !loadedPackagesInProcess.add(packageName))
        //             return;

        //         if ((Boolean) getBooleanField(loadedApk, "mIncludeCode") == false)
        //             return;

        //         LoadPackageParam lpparam = new LoadPackageParam(sLoadedPackageCallbacks);
        //         lpparam.packageName = packageName;
        //         lpparam.processName = AndroidAppHelper.currentProcessName();
        //         lpparam.classLoader = loadedApk.getClassLoader();
        //         lpparam.appInfo = loadedApk.getApplicationInfo();
        //         lpparam.isFirstApplication = false;
        //         XC_LoadPackage.callAll(lpparam);
        //     }
        // });

        disableResources = true;

    }

    private static void hookXposedInstaller(ClassLoader classLoader) {
        try {
            findAndHookMethod(INSTALLER_PACKAGE_NAME + ".XposedApp", classLoader, "getActiveXposedVersion",
                              XC_MethodReplacement.returnConstant(XPOSED_BRIDGE_VERSION));
        } catch (Throwable t) { XposedBridge.log(t); }
    }

    /**
     * Try to load all modules defined in <code>BASE_DIR/conf/modules.list</code>
     */
    private static void loadModules(String startClassName) throws IOException {
        BufferedReader apks = new BufferedReader(new FileReader(BASE_DIR + "conf/modules.list"));
        String apk;
        log(">>>load modules");
        while ((apk = apks.readLine()) != null) {
            log("load modules: "+apk);
            loadModule(apk, startClassName);
        }
        log("<<<load modules");
        apks.close();
    }

    /**
     * Load a module from an APK by calling the init(String) method for all classes defined
     * in <code>assets/xposed_init</code>.
     */
    @SuppressWarnings("deprecation")
    private static void loadModule(String apk, String startClassName) {
        log("Loading modules from " + apk);

        if (!new File(apk).exists()) {
            log("  File does not exist");
            return;
        }

        ClassLoader mcl = new PathClassLoader(apk, BOOTCLASSLOADER);
        InputStream is = mcl.getResourceAsStream("assets/xposed_init");
        if (is == null) {
            log("assets/xposed_init not found in the APK");
            return;
        }

        BufferedReader moduleClassesReader = new BufferedReader(new InputStreamReader(is));
        try {
            String moduleClassName;
            while ((moduleClassName = moduleClassesReader.readLine()) != null) {
                moduleClassName = moduleClassName.trim();
                if (moduleClassName.isEmpty() || moduleClassName.startsWith("#"))
                    continue;

                try {
                    log ("  Loading class " + moduleClassName);
                    Class<?> moduleClass = mcl.loadClass(moduleClassName);

                    if (!IXposedMod.class.isAssignableFrom(moduleClass)) {
                        log ("    This class doesn't implement any sub-interface of IXposedMod, skipping it");
                        continue;
                    } else if (disableResources && IXposedHookInitPackageResources.class.isAssignableFrom(moduleClass)) {
                        log ("    This class requires resource-related hooks (which are disabled), skipping it.");
                        continue;
                    }

                    // call the init(String) method of the module
                    final Object moduleInstance = moduleClass.newInstance();
                    if (startClassName == null) {
                        if (moduleInstance instanceof IXposedHookZygoteInit) {
                            IXposedHookZygoteInit.StartupParam param = new IXposedHookZygoteInit.StartupParam();
                            param.modulePath = apk;
                            ((IXposedHookZygoteInit) moduleInstance).initZygote(param);
                        }

                        if (moduleInstance instanceof IXposedHookLoadPackage)
                            hookLoadPackage(new IXposedHookLoadPackage.Wrapper((IXposedHookLoadPackage) moduleInstance));

                        if (moduleInstance instanceof IXposedHookInitPackageResources)
                            hookInitPackageResources(new IXposedHookInitPackageResources.Wrapper((IXposedHookInitPackageResources) moduleInstance));
                    } else {
                        if (moduleInstance instanceof IXposedHookCmdInit) {
                            IXposedHookCmdInit.StartupParam param = new IXposedHookCmdInit.StartupParam();
                            param.modulePath = apk;
                            param.startClassName = startClassName;
                            ((IXposedHookCmdInit) moduleInstance).initCmdApp(param);
                        }
                    }
                } catch (Throwable t) {
                    log(t);
                }
            }
        } catch (IOException e) {
            log(e);
        } finally {
            try {
                is.close();
            } catch (IOException ignored) {}
        }
    }

    /**
     * Writes a message to the Xposed error log.
     *
     * <p>DON'T FLOOD THE LOG!!! This is only meant for error logging.
     * If you want to write information/debug messages, use logcat.
     *
     * @param text The log message.
     */
    public synchronized static void log(String text) {
        Log.i("Xposed", text);
    }

    /**
     * Logs a stack trace to the Xposed error log.
     *
     * <p>DON'T FLOOD THE LOG!!! This is only meant for error logging.
     * If you want to write information/debug messages, use logcat.
     *
     * @param t The Throwable object for the stack trace.
     */
    public synchronized static void log(Throwable t) {
        Log.i("Xposed", Log.getStackTraceString(t));
    }

    /**
     * Hook any method with the specified callback
     *
     * @param hookMethod The method to be hooked
     * @param callback
     */
    public static XC_MethodHook.Unhook hookMethod(Member hookMethod, XC_MethodHook callback) {
        if (!(hookMethod instanceof Method) && !(hookMethod instanceof Constructor<?>)) {
            throw new IllegalArgumentException("Only methods and constructors can be hooked: " + hookMethod.toString());
        } else if (hookMethod.getDeclaringClass().isInterface()) {
            throw new IllegalArgumentException("Cannot hook interfaces: " + hookMethod.toString());
        } else if (Modifier.isAbstract(hookMethod.getModifiers())) {
            throw new IllegalArgumentException("Cannot hook abstract methods: " + hookMethod.toString());
        }

        boolean newMethod = false;
        CopyOnWriteSortedSet<XC_MethodHook> callbacks;
        synchronized (sHookedMethodCallbacks) {
            callbacks = sHookedMethodCallbacks.get(hookMethod);
            if (callbacks == null) {
                callbacks = new CopyOnWriteSortedSet<XC_MethodHook>();
                sHookedMethodCallbacks.put(hookMethod, callbacks);
                newMethod = true;
            }
        }
        callbacks.add(callback);
        if (newMethod) {
            Class<?> declaringClass = hookMethod.getDeclaringClass();
            // int slot = (int) getIntField(hookMethod, "slot");

            Class<?>[] parameterTypes;
            Class<?> returnType;
            if (hookMethod instanceof Method) {
                parameterTypes = ((Method) hookMethod).getParameterTypes();
                returnType = ((Method) hookMethod).getReturnType();
            } else {
                parameterTypes = ((Constructor<?>) hookMethod).getParameterTypes();
                returnType = null;
            }

            AdditionalHookInfo additionalInfo = new AdditionalHookInfo(callbacks);
            hookMethodNative(hookMethod, additionalInfo);
        }

        return callback.new Unhook(hookMethod);
    }

    /**
     * Removes the callback for a hooked method
     * @param hookMethod The method for which the callback should be removed
     * @param callback The reference to the callback as specified in {@link #hookMethod}
     */
    public static void unhookMethod(Member hookMethod, XC_MethodHook callback) {
        CopyOnWriteSortedSet<XC_MethodHook> callbacks;
        synchronized (sHookedMethodCallbacks) {
            callbacks = sHookedMethodCallbacks.get(hookMethod);
            if (callbacks == null)
                return;
        }
        callbacks.remove(callback);
    }

    public static Set<XC_MethodHook.Unhook> hookAllMethods(Class<?> hookClass, String methodName, XC_MethodHook callback) {
        Set<XC_MethodHook.Unhook> unhooks = new HashSet<XC_MethodHook.Unhook>();
        for (Member method : hookClass.getDeclaredMethods())
            if (method.getName().equals(methodName))
                unhooks.add(hookMethod(method, callback));
        return unhooks;
    }

    public static Set<XC_MethodHook.Unhook> hookAllConstructors(Class<?> hookClass, XC_MethodHook callback) {
        Set<XC_MethodHook.Unhook> unhooks = new HashSet<XC_MethodHook.Unhook>();
        for (Member constructor : hookClass.getDeclaredConstructors())
            unhooks.add(hookMethod(constructor, callback));
        return unhooks;
    }

    /**
     * This method is called as a replacement for hooked methods.
     */
    public static Object handleHookedMethod(Member method, Object additionalInfoObj,
                                             Object thisObject, Object[] args) throws Throwable {
        Log.e("xposed_bridge", "handleHookedMethod");
        AdditionalHookInfo additionalInfo = (AdditionalHookInfo) additionalInfoObj;
        Log.e("xposed_bridge", "method: "+method+" additionalInfo: "+additionalInfoObj+" this: "+ thisObject);
        for (Object o: args) {
            Log.e("xposed_bridge", "args:"+o);
        }

        Object[] callbacksSnapshot = additionalInfo.callbacks.getSnapshot();
        final int callbacksLength = callbacksSnapshot.length;
        if (callbacksLength == 0) {
            return invokeOriginalMethod(method, thisObject, args);
        }

        MethodHookParam param = new MethodHookParam();
        param.method = method;
        param.thisObject = thisObject;
        param.args = args;

        // // call "before method" callbacks
        int beforeIdx = 0;
        do {
            try {
                ((XC_MethodHook) callbacksSnapshot[beforeIdx]).beforeHookedMethod(param);
            } catch (Throwable t) {
                XposedBridge.log(t);

                // reset result (ignoring what the unexpectedly exiting callback did)
                param.setResult(null);
                param.returnEarly = false;
                continue;
            }

            if (param.returnEarly) {
                // skip remaining "before" callbacks and corresponding "after" callbacks
                beforeIdx++;
                break;
            }
        } while (++beforeIdx < callbacksLength);

        if (!param.returnEarly) {
            param.setResult(invokeOriginalMethod(method, param.thisObject, param.args));
        }

        int afterIdx = beforeIdx - 1;
        do {
            Object lastResult =  param.getResult();
            Throwable lastThrowable = param.getThrowable();

            try {
                ((XC_MethodHook) callbacksSnapshot[afterIdx]).afterHookedMethod(param);
            } catch (Throwable t) {
                XposedBridge.log(t);

                // reset to last result (ignoring what the unexpectedly exiting callback did)
                if (lastThrowable == null)
                    param.setResult(lastResult);
                else
                    param.setThrowable(lastThrowable);
            }
        } while (--afterIdx >= 0);

        // // return
        if (param.hasThrowable()) {
            throw param.getThrowable();
        } else {
            return param.getResult();
        }
    }

    /**
     * Get notified when a package is loaded. This is especially useful to hook some package-specific methods.
     */
    public static XC_LoadPackage.Unhook hookLoadPackage(XC_LoadPackage callback) {
        synchronized (sLoadedPackageCallbacks) {
            sLoadedPackageCallbacks.add(callback);
        }
        return callback.new Unhook();
    }

    public static void unhookLoadPackage(XC_LoadPackage callback) {
        synchronized (sLoadedPackageCallbacks) {
            sLoadedPackageCallbacks.remove(callback);
        }
    }

    /**
     * Get notified when the resources for a package are loaded. In callbacks, resource replacements can be created.
     * @return
     */
    public static XC_InitPackageResources.Unhook hookInitPackageResources(XC_InitPackageResources callback) {
        synchronized (sInitPackageResourcesCallbacks) {
            sInitPackageResourcesCallbacks.add(callback);
        }
        return callback.new Unhook();
    }

    public static void unhookInitPackageResources(XC_InitPackageResources callback) {
        synchronized (sInitPackageResourcesCallbacks) {
            sInitPackageResourcesCallbacks.remove(callback);
        }
    }

    private native static boolean initNative();

    private native synchronized static void hookMethodNative(Member method,Object additionalInfo);

    private native static Object invokeOriginalMethod(Member method, Object thisObject, Object[] args);

    public static class CopyOnWriteSortedSet<E> {
        private transient volatile Object[] elements = EMPTY_ARRAY;

        public synchronized boolean add(E e) {
            int index = indexOf(e);
            if (index >= 0)
                return false;

            Object[] newElements = new Object[elements.length + 1];
            System.arraycopy(elements, 0, newElements, 0, elements.length);
            newElements[elements.length] = e;
            Arrays.sort(newElements);
            elements = newElements;
            return true;
        }

        public synchronized boolean remove(E e) {
            int index = indexOf(e);
            if (index == -1)
                return false;

            Object[] newElements = new Object[elements.length - 1];
            System.arraycopy(elements, 0, newElements, 0, index);
            System.arraycopy(elements, index + 1, newElements, index, elements.length - index - 1);
            elements = newElements;
            return true;
        }

        private int indexOf(Object o) {
            for (int i = 0; i < elements.length; i++) {
                if (o.equals(elements[i]))
                    return i;
            }
            return -1;
        }

        public Object[] getSnapshot() {
            return elements;
        }
    }

    private static class AdditionalHookInfo {
        final CopyOnWriteSortedSet<XC_MethodHook> callbacks;
        private AdditionalHookInfo(CopyOnWriteSortedSet<XC_MethodHook> callbacks) {
            this.callbacks = callbacks;
        }
    }
}
